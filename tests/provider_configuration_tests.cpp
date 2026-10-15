#include "common/provider/provider_setup_input.h"
#include "test_harness.h"
#include <barrier>
#include <thread>
#include "app/provider_configuration_service.h"
#include "app/central_instruction_file_service.h"
#if defined(__APPLE__)
#include <sys/acl.h>
#include <sys/xattr.h>
#include <membership.h>
#endif
#include "common/config/central_provider_configuration.h"
#include "common/config/settings_store.h"
#include "remote/runner_state.h"
#include "remote/runner_client.h"
#include "app/uam_control_service.h"
#include "cef/state_serializer.h"
#include "common/runtime/terminal/terminal_provider_cli.h"
#include "common/runtime/acp/acp_session_internal.h"

using namespace uam_test;

#if defined(__APPLE__)
namespace
{
	/// <summary>Private transport and an unrelated pending check keep launch tests off real SSH/providers.</summary>
	struct RemoteOpenCodeVersionLaunchFixture
	{
		TempDir temp{"uam-opencode-version-launch"};
		ScopedEnvVar path{"PATH", temp.root.string() + ":/usr/bin:/bin"};
		uam::AppState app;
		ExecutionHost host{"private-opencode", "Private", "ssh", "private-fixture", "ready", "development", "linux", "x86_64", "", "runner", 3};
		RemoteOpenCodeVersionLaunchFixture()
		{
			UAM_ASSERT(uam::io::WriteTextFile(temp.root / "ssh", "#!/bin/sh\nexit 0\n"));
			fs::permissions(temp.root / "ssh", fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
			app.data_root = temp.root / "data";
			app.provider_profiles = {ProviderProfileStore::DefaultOpenCodeProfile()};
			app.settings.git_auto_worktree = false;
			app.settings.central_provider_configuration.uam_control_enabled = false;
			app.settings.execution_hosts.push_back(host);
			ChatSession chat;
			chat.id = "private-opencode-launch"; chat.provider_id = "opencode-cli";
			chat.execution_host_id = host.id; chat.workspace_directory = temp.root.string();
			chat.remote_process_control_token = "private-opencode-owner";
			app.chats.push_back(chat);
			app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
			Session().chat_id = chat.id; Session().provider_id = chat.provider_id;
			app.runtime_cli_version_check_task.running = true;
			app.runtime_cli_version_check_task.state = std::make_shared<AsyncProcessTaskState>();
			app.runtime_cli_version_check_task.execution_host = ExecutionHost{};
			app.runtime_cli_version_provider_id = "codex-cli";
		}
		~RemoteOpenCodeVersionLaunchFixture()
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(Session(), true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(Session());
		}
		uam::AcpSessionState& Session() { return *app.acp_sessions.front(); }
		bool Start(std::string* error = nullptr) { return uam::acp_detail::StartAcpProcessForChat(app, Session(), app.chats.front(), error); }
		void Checked(std::string version = "1.17.20")
		{
			uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", host.id)];
			state.execution_host = host; state.checked = true; state.supported = true; state.installed_version = std::move(version);
		}
	};
}
#endif

UAM_TEST(RemoteOpenCodeUncachedLaunchWaitsAndUsesItsCompletedVersionDespiteUnrelatedCheck)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr);
	UAM_ASSERT(!fixture.Session().running);
	UAM_ASSERT(!fixture.app.chats.front().remote_process_exists);
	UAM_ASSERT(fixture.Session().provider_configuration_directory.empty());
	UAM_ASSERT(fixture.Start());
	const std::pair<std::string, std::string> target{"opencode-cli", fixture.host.id};
	UAM_ASSERT_EQ(std::ranges::count(fixture.app.runtime_cli_version_check_queue, target), 1);
	fixture.Checked(); // The existing async version service publishes only metadata at completion.
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
	UAM_ASSERT(fixture.Session().running);
	const nlohmann::json manifest = nlohmann::json::parse(ReadFile(fixture.Session().provider_configuration_directory / "owner.json"));
	UAM_ASSERT(manifest.value("openCodeCodeMode", false));
	UAM_ASSERT_EQ(fixture.app.runtime_cli_version_provider_id, std::string("codex-cli"));
#endif
}

UAM_TEST(RemoteOpenCodeForeignVersionCacheCannotAuthorizeLaunchAndHostEditRevokesPending)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	fixture.Checked();
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", fixture.host.id)].execution_host.ssh_alias = "foreign-fixture";
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr);
	UAM_ASSERT(!fixture.Session().running);
	fixture.app.settings.execution_hosts.back().ssh_alias = "replacement-fixture";
	std::string error;
	UAM_ASSERT(!fixture.Start(&error));
	UAM_ASSERT(!error.empty());
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
	UAM_ASSERT(!fixture.Session().running);
#endif
}

UAM_TEST(RemoteOpenCodeCanceledVersionPreparationCannotLaunchAfterCheckCompletes)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr);
	UAM_ASSERT(uam::StopAcpSession(fixture.app, fixture.app.chats.front().id, uam::AcpStopPurpose::Interrupt, true));
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
	fixture.Checked();
	uam::PollAllAcpSessions(fixture.app);
	UAM_ASSERT(!fixture.Session().running);
	UAM_ASSERT(!fixture.app.chats.front().remote_process_exists);
#endif
}

UAM_TEST(RemoteOpenCodeCompletedUnknownVersionKeepsEagerLaunchCompatibility)
{
#if defined(__APPLE__)
	for (const std::string version : {"1.18.36", "1.17.19", ""})
	{
		RemoteOpenCodeVersionLaunchFixture fixture;
		fixture.Checked(version);
		fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", fixture.host.id)].supported = false;
		UAM_ASSERT(fixture.Start());
		UAM_ASSERT(fixture.Session().running);
		UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
		UAM_ASSERT(fixture.Session().provider_configuration_directory.empty());
	}
#endif
}

UAM_TEST(RemoteOpenCodeMatchingVersionCacheLaunchesWithoutWaitingForUnrelatedCheck)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	fixture.Checked();
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().running && fixture.Session().pending_local_launch == nullptr);
	UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.empty());
	UAM_ASSERT_EQ(fixture.app.runtime_cli_version_provider_id, std::string("codex-cli"));
	const nlohmann::json manifest = nlohmann::json::parse(ReadFile(fixture.Session().provider_configuration_directory / "owner.json"));
	UAM_ASSERT(manifest.value("openCodeCodeMode", false));
#endif
}

UAM_TEST(RemoteOpenCodeVersionHealthPublicationPreservesPendingLaunchOwnership)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	fixture.app.runtime_cli_version_check_task.execution_host = fixture.host;
	fixture.app.runtime_cli_version_provider_id = "opencode-cli";
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr && !fixture.Session().running);
	UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.empty());
	// The normal application tick can consume this owned worker after ACP polling, before its next call.
	const std::shared_ptr<AsyncProcessTaskState> result = fixture.app.runtime_cli_version_check_task.state;
	result->result.ok = true;
	result->result.exit_code = 0;
	result->result.output = "1.17.20\n";
	result->remote_helper_connected = true;
	result->completed.store(true, std::memory_order_release);
	ProviderCliCompatibilityService().Poll(fixture.app);
	UAM_ASSERT(!fixture.app.settings.execution_hosts.back().last_seen_at.empty());
	UAM_ASSERT(fixture.app.runtime_cli_versions_by_provider_id.at(CliProviderVersionStateKey("opencode-cli", fixture.host.id)).checked);
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr && fixture.Session().running);
	const nlohmann::json manifest = nlohmann::json::parse(ReadFile(fixture.Session().provider_configuration_directory / "owner.json"));
	UAM_ASSERT(manifest.value("openCodeCodeMode", false));
#endif
}

UAM_TEST(RemoteOpenCodeVersionWaitRejectsChangedCentralConfiguration)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr);
	fixture.app.settings.central_provider_configuration.enabled = true;
	fixture.Checked();
	std::string error;
	UAM_ASSERT(!fixture.Start(&error));
	UAM_ASSERT(error.find("ownership changed") != std::string::npos);
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr && !fixture.Session().running);
	UAM_ASSERT(!fixture.app.chats.front().remote_process_exists);
#endif
}

UAM_TEST(RemoteOpenCodeFailedVersionCheckHoldsRawQueueUntilExplicitRetry)
{
#if defined(__APPLE__)
	RemoteOpenCodeVersionLaunchFixture fixture;
	uam::AcpQueuedUserPromptState raw;
	raw.id = "private-version-input"; raw.revision = 7; raw.text = "Private raw input";
	raw.markdown_store_prompt_blocks = {"EXACT_CAPTURED_PRIVATE_SKILL"};
	fixture.Session().queued_user_prompts = {raw};
	fixture.app.chats.front().acp_queued_prompts = {raw};
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr);
	fixture.Checked("");
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", fixture.host.id)].check_error = "Private version transport failed";
	std::string error;
	UAM_ASSERT(!fixture.Start(&error));
	UAM_ASSERT(error.find("Private version transport failed") != std::string::npos);
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
	UAM_ASSERT(fixture.Session().queue_held && fixture.app.chats.front().acp_queue_held);
	UAM_ASSERT(!fixture.Session().reconnect_pending && !fixture.Session().running);
	uam::PollAllAcpSessions(fixture.app);
	UAM_ASSERT(fixture.Session().pending_local_launch == nullptr && !fixture.Session().running);
	UAM_ASSERT(fixture.Session().queued_user_prompts.front() == raw);
	UAM_ASSERT(fixture.app.chats.front().acp_queued_prompts.front() == raw);
	UAM_ASSERT(fixture.app.pending_chat_save_at_by_chat_id.contains(fixture.app.chats.front().id));
	UAM_ASSERT(fixture.Start()); // A new explicit attempt may refresh the failed check, but sends nothing.
	UAM_ASSERT(fixture.Session().pending_local_launch != nullptr && !fixture.Session().running);
	fixture.Checked();
	fixture.app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", fixture.host.id)].check_error.clear();
	UAM_ASSERT(fixture.Start());
	UAM_ASSERT(fixture.Session().running && fixture.Session().queue_held);
	UAM_ASSERT(fixture.Session().prompt_request_id == 0);
	UAM_ASSERT(fixture.Session().queued_user_prompts.front() == raw);
#endif
}

UAM_TEST(RemoteOpenCodeWarmAndRecoveryLaunchesDoNotProbeOrReconfigure)
{
#if defined(__APPLE__)
	for (const bool recovery : {false, true})
	{
		RemoteOpenCodeVersionLaunchFixture fixture;
		fixture.Session().running = !recovery;
		fixture.Session().recovering_remote_process = recovery;
		UAM_ASSERT(fixture.Start());
		UAM_ASSERT(fixture.Session().pending_local_launch == nullptr);
		UAM_ASSERT(fixture.app.runtime_cli_version_check_queue.empty());
		UAM_ASSERT(fixture.Session().provider_configuration_directory.empty());
		if (!recovery) fixture.Session().running = false; // No OS process belongs to the warm-state fixture.
	}
#endif
}

UAM_TEST(OpenCodeDiscoveryUsesOnlyCheckedVersionsOnTheSelectedHost)
{
	uam::AppState app;
	ExecutionHost local;
	ExecutionHost remote;
	remote.id = "ssh-fixture";
	remote.transport = "ssh";
	remote.ssh_alias = "fixture";
	uam::CliProviderVersionState& state = app.runtime_cli_versions_by_provider_id[CliProviderVersionStateKey("opencode-cli", remote.id)];
	state.execution_host = remote;
	state.installed_version = "1.17.20";
	UAM_ASSERT(uam::CheckedProviderConfigurationVersion(app, "opencode-cli", remote).empty());
	state.checked = true;
	UAM_ASSERT(uam::CheckedProviderConfigurationVersion(app, "opencode-cli", remote).empty());
	state.supported = true;
	UAM_ASSERT_EQ(uam::CheckedProviderConfigurationVersion(app, "opencode-cli", remote), std::string("1.17.20"));
	UAM_ASSERT(uam::CheckedProviderConfigurationVersion(app, "opencode-cli", local).empty());
	UAM_ASSERT(uam::CheckedProviderConfigurationVersion(app, "codex-cli", remote).empty());
	remote.ssh_alias = "different-target";
	UAM_ASSERT(uam::CheckedProviderConfigurationVersion(app, "opencode-cli", remote).empty());
}

UAM_TEST(OpenCodeDiscoverySupportsInheritedMcpWithoutChangingOtherProviderBundles)
{
	TempDir temp("uam-discovery-versions");
	AppSettings settings;
	ChatSession chat;
	chat.provider_id = "opencode-cli";
	ExecutionHost host;
	for (const std::string version : {"", "1.17.19", "1.17.20", "1.18.35", "1.18.36", "unknown"})
	{
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, nullptr, version));
		const bool verified = version == "1.17.20" || version == "1.18.35";
		UAM_ASSERT_EQ(!bundle.local_directory.empty(), verified);
		if (!verified) continue;
		UAM_ASSERT_EQ(bundle.manifest["format"], nlohmann::json(1));
		UAM_ASSERT(bundle.manifest.value("openCodeCodeMode", false));
		UAM_ASSERT(bundle.manifest.value("mcpOnly", false));
		UAM_ASSERT(bundle.manifest["mcpServers"].empty());
		UAM_ASSERT(bundle.files.empty());
	}
	settings.central_provider_configuration.enabled = true;
	for (const std::string provider : {"codex-cli", "claude-cli", "gemini-cli", "copilot-cli", "antigravity-cli"})
	{
		chat.provider_id = provider;
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, nullptr, "1.17.20"));
		UAM_ASSERT(!bundle.manifest.contains("openCodeCodeMode"));
	}
}

UAM_TEST(OpenCodeDiscoveryManifestRequiresBooleanHintAndRetainsFormatOne)
{
	nlohmann::json manifest = {{"format", 1}, {"files", nlohmann::json::object()}};
	std::string error;
	for (const nlohmann::json hint : {nlohmann::json(true), nlohmann::json(false)})
	{
		manifest["openCodeCodeMode"] = hint;
		UAM_ASSERT(uam::provider_setup::ValidateManifest(manifest, error));
	}
	for (const nlohmann::json hint : {nlohmann::json("true"), nlohmann::json(1), nlohmann::json(nullptr), nlohmann::json::object()})
	{
		manifest["openCodeCodeMode"] = hint;
		UAM_ASSERT(!uam::provider_setup::ValidateManifest(manifest, error));
	}
}

UAM_TEST(OpenCodeDiscoveryPreservesExecutingHostAndLaunchEnvironmentOverrides)
{
	TempDir temp("uam-discovery-environment");
	constexpr const char* flag = "OPENCODE_EXPERIMENTAL_CODE_MODE";
	ScopedEnvVar original(flag, "");
#if !defined(_WIN32)
	unsetenv(flag);
#endif
	AppSettings settings;
	ChatSession chat;
	chat.provider_id = "opencode-cli";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, nullptr, "1.17.20"));
	std::vector<std::string> argv{"opencode", "acp"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT_EQ(uam::provider_setup::EnvironmentValue(environment, flag), std::string("true"));
	UAM_ASSERT(std::getenv(flag) == nullptr);
	UAM_ASSERT_EQ(argv, std::vector<std::string>({"opencode", "acp"}));
	for (const std::string value : {"true", "false", ""})
	{
		environment = {{flag, value}, {"OPENCODE_PERMISSION", R"({"read":"deny"})"}};
		UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
		UAM_ASSERT_EQ(uam::provider_setup::EnvironmentValue(environment, flag), value);
		UAM_ASSERT_EQ(std::count_if(environment.begin(), environment.end(), [flag](const std::pair<std::string, std::string>& entry) { return entry.first == flag; }), 1);
		UAM_ASSERT_EQ(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_PERMISSION"), std::string(R"({"read":"deny"})"));
		ScopedEnvVar inherited(flag, value);
		environment.clear();
		UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
#if defined(_WIN32)
		if (value.empty()) continue; // The CRT removes an empty assignment; explicit launch entries above still preserve it.
#endif
		UAM_ASSERT(std::ranges::none_of(environment, [flag](const std::pair<std::string, std::string>& entry) { return entry.first == flag; }));
	}
#if defined(_WIN32)
	environment = {{"opencode_experimental_code_mode", "false"}};
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT_EQ(environment.front().second, std::string("false"));
	UAM_ASSERT(std::ranges::none_of(environment, [flag](const std::pair<std::string, std::string>& entry) { return entry.first == flag; }));
#endif
	argv = {"claude"};
	environment.clear();
	UAM_ASSERT(uam::provider_setup::Apply("claude-cli", bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(std::ranges::none_of(environment, [flag](const std::pair<std::string, std::string>& entry) { return entry.first == flag; }));
}

UAM_TEST(OpenCodeDiscoveryRunnerPropagatesOnlyTheChildDefault)
{
	TempDir temp("uam-discovery-runner");
	constexpr const char* flag = "OPENCODE_EXPERIMENTAL_CODE_MODE";
	ScopedEnvVar original(flag, "");
#if !defined(_WIN32)
	unsetenv(flag);
#endif
	AppSettings settings;
	ChatSession chat;
	chat.provider_id = "opencode-cli";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, nullptr, "1.17.20"));
	const fs::path target = temp.root / ".UAM/provider-setup" / bundle.local_directory.filename();
	std::vector<std::string> missing;
	UAM_ASSERT(uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
#if defined(_WIN32)
	const std::vector<std::string> argv{"cmd.exe", "/D", "/C", "echo %OPENCODE_EXPERIMENTAL_CODE_MODE%"};
#else
	const std::vector<std::string> argv{"/bin/sh", "-c", "printf %s \"$OPENCODE_EXPERIMENTAL_CODE_MODE\""};
#endif
	uam::remote::RunnerState runner;
	const nlohmann::json started = runner.HandleProcessRequest({{"id", "start"}, {"type", "process.start"}, {"sessionId", "discovery-fixture"},
	    {"controlToken", "private-token"}, {"cwd", uam::paths::Utf8PathString(temp.root)}, {"argv", argv},
	    {"configurationProvider", chat.provider_id}, {"configurationDirectory", uam::paths::Utf8PathString(target)}});
	UAM_ASSERT(started.value("ok", false));
	std::string output;
	bool exited = false;
	for (int attempt = 0; attempt < 200; ++attempt)
	{
		const nlohmann::json polled = runner.HandleProcessRequest({{"id", "poll"}, {"type", "process.poll"}, {"sessionId", "discovery-fixture"}, {"controlToken", "private-token"}});
		UAM_ASSERT(polled.value("ok", false));
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(polled["result"].value("stdoutBase64", ""), chunk));
		output += chunk;
		if (!polled["result"].value("running", true)) { exited = true; break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	UAM_ASSERT_EQ(uam::strings::Trim(output), std::string("true"));
	UAM_ASSERT(std::getenv(flag) == nullptr);
	UAM_ASSERT(runner.HandleProcessRequest({{"id", "remove"}, {"type", "process.remove"}, {"sessionId", "discovery-fixture"}, {"controlToken", "private-token"}}).value("ok", false));
}

UAM_TEST(CentralMemoryDetailsRemainReadableThroughTheOwnedIndex)
{
	TempDir temp("uam-central-memory-details");
	const fs::path data = temp.root / "data";
	const fs::path memory = MemoryService::GlobalMemoryRoot(data) / uam::memory::kLessonsAi;
	fs::create_directories(memory);
	const std::string detail = "# Example\nA short lesson.\n\n## Evidence\nDetailed source retained only in the owned file.\n";
	UAM_ASSERT(uam::io::WriteTextFile(memory / "example.md", detail));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	ChatSession chat;
	chat.provider_id = "opencode-cli";
	chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(data, settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(bundle.files.at("memory/global/Lessons/AI_Lessons/example.md"), detail);
	UAM_ASSERT(bundle.files.at("memory/index.md").find("global/Lessons/AI_Lessons/example.md") != std::string::npos);
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("__UAM_RESOURCE_ROOT__/memory/index.md") != std::string::npos);
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("Detailed source retained") == std::string::npos);
	std::vector<std::string> argv{"opencode", "acp"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	const nlohmann::json config = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
	const fs::path instructions = uam::paths::PathFromUtf8(config["instructions"][0].get<std::string>());
	UAM_ASSERT(ReadFile(instructions).find(uam::paths::Utf8PathString(bundle.local_directory / "memory/index.md")) != std::string::npos);
	UAM_ASSERT_EQ(ReadFile(bundle.local_directory / "memory/global/Lessons/AI_Lessons/example.md"), detail);
	UAM_ASSERT_EQ(ReadFile(memory / "example.md"), detail);
}

UAM_TEST(CentralMemoryDetailFailureKeepsTheOriginalLaunchAndNoBrokenIndex)
{
	TempDir temp("uam-central-memory-fallback");
	const fs::path data = temp.root / "data";
	const fs::path memory = MemoryService::GlobalMemoryRoot(data) / uam::memory::kLessonsAi;
	fs::create_directories(memory);
	UAM_ASSERT(uam::io::WriteTextFile(memory / "large.md", std::string(uam::provider_setup::kMaxFileBytes + 1, 'x')));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	ChatSession chat;
	chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(data, settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(!bundle.files.contains("memory/index.md"));
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("Memory details:") == std::string::npos);
	UAM_ASSERT(error.empty());
}

UAM_TEST(CentralMemoryManifestOverflowKeepsTheOriginalLaunch)
{
	TempDir temp("uam-central-memory-manifest-fallback");
	const fs::path data = temp.root / "data";
	const fs::path memory = MemoryService::GlobalMemoryRoot(data) / uam::memory::kLessonsAi;
	fs::create_directories(memory);
	for (int index = 0; index < 1900; ++index)
		UAM_ASSERT(uam::io::WriteTextFile(memory / (std::to_string(index) + "-" + std::string(215, 'x') + ".md"), "A bounded fact."));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	ChatSession chat;
	chat.workspace_directory = uam::paths::Utf8PathString(temp.root);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(data, settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(!bundle.files.contains("memory/index.md"));
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("Memory details:") == std::string::npos);
	UAM_ASSERT(uam::provider_setup::ValidateManifest(bundle.manifest, error));
	UAM_ASSERT(bundle.manifest.dump().size() < 512 * 1024);
	UAM_ASSERT_EQ(ReadFile(memory / ("0-" + std::string(215, 'x') + ".md")), std::string("A bounded fact."));
}

UAM_TEST(CentralConfigurationPersistsAndRejectsMalformedPaths)
{
	TempDir temp("uam-central-settings");
	AppSettings settings;
	settings.central_provider_configuration = {true, "Shared instructions", {(temp.root / "AGENTS.md").string()}, {(temp.root / "skills").string()}, "plan", true};
	const fs::path path = temp.root / "settings.txt";
	UAM_ASSERT(SettingsStore::Save(path, settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(path, loaded).loaded);
	UAM_ASSERT_EQ(uam::central_configuration::Serialize(loaded.central_provider_configuration), uam::central_configuration::Serialize(settings.central_provider_configuration));
	std::string error;
	const nlohmann::json malformed = {{"instructionFiles", nlohmann::json::array({"relative.md"})}};
	UAM_ASSERT(!uam::central_configuration::Parse(malformed, loaded.central_provider_configuration, error));
	UAM_ASSERT_EQ(loaded.central_provider_configuration.instructions, std::string("Shared instructions"));
}

UAM_TEST(CentralResourcesPreserveFullSkillAssetsAndDetectTampering)
{
	TempDir temp("uam-central-assets");
	fs::create_directories(temp.root / "skills" / "example" / "references");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skills/example/SKILL.md", "Read references/data.bin"));
	const std::string binary("a\0b", 3);
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skills/example/references/data.bin", binary));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.skill_directories = {(temp.root / "skills").string()};
	ChatSession chat;
	chat.id = "chat";
	chat.uam_agent_id = "build";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	if (!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
	UAM_ASSERT_EQ(ReadFile(bundle.local_directory / "skills/example/references/data.bin"), binary);
	std::vector<std::string> missing;
	UAM_ASSERT(uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, error));
	UAM_ASSERT(missing.empty());
	UAM_ASSERT(uam::io::WriteTextFile(bundle.local_directory / "skills/example/SKILL.md", "changed"));
	UAM_ASSERT(!uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, error));
	UAM_ASSERT(error.find("differs") != std::string::npos);
	UAM_ASSERT(!uam::provider_setup::SafeRelativePath("../outside"));
	UAM_ASSERT(!uam::provider_setup::SafeRelativePath("C:\\outside"));
}

UAM_TEST(CentralSkillManifestPathsStayPortableForDirectoriesAndImportedLibraries)
{
	TempDir temp("uam-central-portable-assets");
	const fs::path skill = temp.root / "example";
	const std::string relative_asset = "references/nested/caf\xc3\xa9.bin";
	const fs::path asset = skill / uam::paths::PathFromUtf8(relative_asset);
	fs::create_directories(asset.parent_path());
	UAM_ASSERT(uam::io::WriteTextFile(skill / "SKILL.md", "Read the nested reference."));
	const std::string binary("a\0b", 3);
	UAM_ASSERT(uam::io::WriteTextFile(asset, binary));
	for (const bool imported : {false, true})
	{
		AppSettings settings;
		settings.central_provider_configuration.enabled = true;
		std::string skill_name = "example";
		std::string error;
		if (imported)
		{
			const fs::path library = temp.root / "library";
			fs::create_directories(library);
			const std::vector<MarkdownStoreService::ImportResult> results = MarkdownStoreService::ImportEntries(
			    library, {{"claude", skill / "SKILL.md", MarkdownStoreService::ImportConflictAction::Skip}}, &error);
			UAM_ASSERT_EQ(results.size(), std::size_t(1));
			UAM_ASSERT_EQ(results.front().status, std::string("imported"));
			settings.markdown_store_directory = uam::paths::Utf8PathString(library);
			skill_name = results.front().entry.command_name;
			settings.central_provider_configuration.default_skills = {skill_name};
		}
		else settings.central_provider_configuration.skill_directories = {uam::paths::Utf8PathString(skill)};
		ChatSession chat;
		ExecutionHost host;
		uam::ProviderConfigurationBundle bundle;
		if (!uam::PrepareProviderConfiguration(temp.root / (imported ? "imported-data" : "direct-data"), settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
		const std::string key = "skills/" + skill_name + "/" + relative_asset;
		UAM_ASSERT(bundle.files.contains(key));
		UAM_ASSERT_EQ(bundle.files.at(key), binary);
		UAM_ASSERT(bundle.manifest["files"].contains(key));
		UAM_ASSERT_EQ(ReadFile(bundle.local_directory / uam::paths::PathFromUtf8(key)), binary);
		for (const std::pair<const std::string, std::string>& file : bundle.files)
		{
			UAM_ASSERT(file.first.find('\\') == std::string::npos);
			UAM_ASSERT(uam::provider_setup::SafeRelativePath(file.first));
		}
	}
}

UAM_TEST(CentralLaunchContractsCoverAllFiveProvidersAndPreserveOpenCodeConfig)
{
	TempDir temp("uam-central-contracts");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Use shared instructions";
	ChatSession chat;
	chat.id = "chat";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	if (!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error)) throw std::runtime_error(error);
	for (const std::string& provider : {"codex-cli", "claude-cli", "gemini-cli", "opencode-cli", "copilot-cli"})
	{
		std::vector<std::string> argv{provider};
		uam::provider_setup::Environment environment;
		if (provider == "opencode-cli") environment.emplace_back("OPENCODE_CONFIG_CONTENT", R"({"theme":"existing","instructions":["existing.md"],"permission":{"edit":"deny"}})");
		UAM_ASSERT(uam::provider_setup::Apply(provider, bundle.local_directory, temp.root, argv, environment, error));
		if (provider == "codex-cli") UAM_ASSERT(std::ranges::any_of(argv, [](const std::string& arg) { return arg.starts_with("developer_instructions=") && arg.find("Use shared instructions") != std::string::npos && arg.find("__UAM_RESOURCE_ROOT__") == std::string::npos; }));
		if (provider == "claude-cli") UAM_ASSERT(std::ranges::find(argv, "--append-system-prompt-file") != argv.end());
		if (provider == "gemini-cli") UAM_ASSERT(!uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH").empty());
		if (provider == "copilot-cli") UAM_ASSERT(!uam::provider_setup::EnvironmentValue(environment, "COPILOT_CUSTOM_INSTRUCTIONS_DIRS").empty());
		if (provider == "opencode-cli")
		{
			const nlohmann::json configuration = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
			UAM_ASSERT_EQ(configuration["theme"], nlohmann::json("existing"));
			UAM_ASSERT_EQ(configuration["permission"]["edit"], nlohmann::json("deny"));
			UAM_ASSERT_EQ(configuration["instructions"].size(), std::size_t{2});
		}
	}
}

UAM_TEST(CodexCentralSetupLeavesWorkspaceInstructionsToNativeDiscovery)
{
	TempDir temp("uam-codex-native-workspace-instructions");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Shared central preference";
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCodexCli;
	ExecutionHost host;
	const std::string workspace_instructions = "Workspace-only marker\n" + std::string(128 * 1024, 'x');
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "AGENTS.md", workspace_instructions));
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	std::vector<std::string> argv{"codex", "app-server", "--listen", "stdio://"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(std::ranges::any_of(argv, [](const std::string& argument) { return argument.starts_with("developer_instructions=") && argument.find("Shared central preference") != std::string::npos; }));
	UAM_ASSERT(std::ranges::none_of(argv, [](const std::string& argument) { return argument.find("Workspace-only marker") != std::string::npos || argument.size() > 12 * 1024; }));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(temp.root / "AGENTS.md"), workspace_instructions);
	argv = {"claude"};
	UAM_ASSERT(uam::provider_setup::Apply(uam::provider_ids::kClaudeCli, bundle.local_directory, temp.root, argv, environment, error));
	const std::vector<std::string>::iterator file_flag = std::ranges::find(argv, "--append-system-prompt-file");
	UAM_ASSERT(file_flag != argv.end() && std::next(file_flag) != argv.end());
	UAM_ASSERT(uam::io::ReadTextFile(uam::paths::PathFromUtf8(*std::next(file_flag))).find(workspace_instructions) != std::string::npos);
}

UAM_TEST(CentralSshPreparationIsResumableAndRejectsUnownedDirectories)
{
	TempDir temp("uam-central-ssh");
	uam::remote::RunnerState runner;
	const std::string bytes = "shared";
	nlohmann::json manifest = {{"format", 1}, {"files", {{"AGENTS.md", {{"size", bytes.size()}, {"digest", uam::provider_setup::Digest(bytes)}}}}}};
	const fs::path directory = temp.root / ".UAM/provider-setup" / uam::provider_setup::Digest(manifest.dump());
	const nlohmann::json request = {{"id", "configuration"}, {"type", "configuration.prepare"}, {"directory", directory.string()}, {"manifest", manifest}};
	const nlohmann::json prepared = runner.HandleProcessRequest(request);
	if (!prepared.value("ok", false)) throw std::runtime_error(prepared.dump());
	UAM_ASSERT_EQ(prepared["result"]["missing"].size(), std::size_t{1});
	UAM_ASSERT(uam::io::WriteTextFile(directory / "AGENTS.md", bytes));
	UAM_ASSERT(runner.HandleProcessRequest(request)["result"]["missing"].empty());
	manifest["files"]["../outside"] = {{"size", std::size_t{0}}, {"digest", uam::provider_setup::Digest("")}};
	UAM_ASSERT(!runner.HandleProcessRequest({{"id", "configuration"}, {"type", "configuration.prepare"}, {"directory", directory.string()}, {"manifest", manifest}}).value("ok", false));
}

UAM_TEST(CentralMcpScopeOverridesGlobalsAndResolvesSecretsOnlyAtLaunch)
{
	TempDir temp("uam-central-mcp-secrets");
	McpServerConfiguration global;
	global.id = "global";
	global.name = "example";
	global.transport = "http";
	global.url = "http://localhost:5000/mcp";
	global.headers = {{"Authorization", "UAM_CENTRAL_TEST_SECRET"}};
	McpServerConfiguration remote = global;
	remote.id = "remote";
	remote.execution_host_id = "ssh-test";
	remote.url = "http://localhost:6000/mcp";
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.mcp_servers = {remote, global};
	ExecutionHost host;
	host.id = "ssh-test";
	host.transport = "ssh";
	ChatSession chat;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	ScopedEnvVar unset_secret("UAM_CENTRAL_TEST_SECRET", "");
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"].size(), std::size_t{1});
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["url"], nlohmann::json(remote.url));
	std::vector<std::string> argv{"codex"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(!uam::provider_setup::Apply("codex-cli", bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT(error.find("UAM_CENTRAL_TEST_SECRET") != std::string::npos);
	ScopedEnvVar target_secret("UAM_CENTRAL_TEST_SECRET", "target-only-value");
	argv = {"codex"};
	UAM_ASSERT(uam::provider_setup::Apply("codex-cli", bundle.local_directory, temp.root, argv, environment, error));
	for (const std::string& argument : argv) UAM_ASSERT(argument.find("target-only-value") == std::string::npos);
	UAM_ASSERT(bundle.manifest.dump().find("target-only-value") == std::string::npos);
	UAM_ASSERT(std::ranges::any_of(environment, [](const auto& entry) { return entry.second == "target-only-value"; }));
}

#if !defined(_WIN32)
UAM_TEST(CentralSshClientTransfersRealAssetsAndPreservesHostContext)
{
	TempDir temp("uam-central-real-runner");
	IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
	const fs::path runner = service.ResolveCurrentExecutablePath().parent_path() / "uam-runner";
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Controller instruction";
	ChatSession chat;
	ExecutionHost host;
	host.id = "ssh-test";
	host.transport = "ssh";
	host.instruction_file = (temp.root / "host.md").string();
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "host.md", "Host instruction"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "AGENTS.md", "Workspace instruction"));
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	const fs::path target = temp.root / ".UAM/provider-setup" / bundle.local_directory.filename();
	uam::remote::RunnerClient client(service, {runner.string(), "bridge-direct"});
	if (!client.InstallProviderConfiguration(bundle.local_directory, target, &error)) throw std::runtime_error(error);
	UAM_ASSERT(client.InstallProviderConfiguration(bundle.local_directory, target, &error));
	std::vector<std::string> argv{"claude"};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply("claude-cli", target, temp.root, argv, environment, error));
	const std::string installed = ReadFile(uam::paths::PathFromUtf8(argv.back()));
	UAM_ASSERT(installed.find("Controller instruction") != std::string::npos);
	UAM_ASSERT(installed.find("Host instruction") != std::string::npos);
	UAM_ASSERT(installed.find("Workspace instruction") != std::string::npos);
	UAM_ASSERT(installed.find(target.string()) != std::string::npos);
	client.Disconnect();
}

UAM_TEST(CentralResourcesRejectSymlinkedAssetsAndPreserveClaudeHandoff)
{
	TempDir temp("uam-central-symbolic-links");
	fs::create_directories(temp.root / "skill");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/SKILL.md", "Skill"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "private.md", "Outside"));
	fs::create_symlink(temp.root / "private.md", temp.root / "skill/reference.md");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.skill_directories = {(temp.root / "skill").string()};
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle rejected;
	std::string error;
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, rejected, error));
	settings.central_provider_configuration.skill_directories.clear();
	uam::ProviderConfigurationBundle bundle;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	std::vector<std::string> argv{"claude", "--append-system-prompt-file", (temp.root / "private.md").string()};
	uam::provider_setup::Environment environment;
	UAM_ASSERT(uam::provider_setup::Apply("claude-cli", bundle.local_directory, temp.root, argv, environment, error));
	UAM_ASSERT_EQ(std::count(argv.begin(), argv.end(), "--append-system-prompt-file"), 1);
	UAM_ASSERT(ReadFile(uam::paths::PathFromUtf8(argv.back())).find("Outside") != std::string::npos);
}
#endif

UAM_TEST(CentralNativeUamControlAuthorityEndsWithTerminalAndKeepsChatCapability)
{
	TempDir temp("uam-central-native-control");
	uam::AppState app;
	app.data_root = temp.root;
	ChatSession chat;
	chat.id = "native-control";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	std::string error;
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& structured = *app.acp_sessions.back();
	structured.chat_id = chat.id;
	structured.provider_id = chat.provider_id;
	structured.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, structured, chat, "session/new", setup, &error));
	const std::string structured_id = structured.uam_control_capability_id;
	app.cli_terminals.push_back(std::make_unique<uam::CliTerminalState>());
	uam::CliTerminalState& terminal = *app.cli_terminals.back();
	nlohmann::json server;
	UAM_ASSERT(uam::UamControlService::PrepareTerminalMcpServer(app, terminal, chat, server, error));
	terminal.running = true;
	const std::string capability_id = terminal.uam_control_session->uam_control_capability_id;
	const nlohmann::json request = {{"requestId", "native-skill-list"}, {"method", "skill_list"}, {"arguments", nlohmann::json::object()}};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, capability_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", false));
	UAM_ASSERT(capability_id != structured_id);
	structured.chat_id = "other-chat";
	const nlohmann::json question = {{"requestId", "native-question"}, {"method", "user_question"}, {"arguments", {{"question", "Proceed?"}, {"options", {"Yes", "No"}}}}};
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, capability_id, question, uam::time::SystemEpochMillisecondsNow()).value("pendingApproval", false));
	const nlohmann::json state = uam::StateSerializer::Serialize(app, true);
	UAM_ASSERT_EQ(state["chats"][0]["acpSession"]["pendingUserInput"]["questions"][0]["question"], nlohmann::json("Proceed?"));
	const auto pending = uam::UamControlService::PendingApprovalForChat(app, chat.id);
	UAM_ASSERT(pending.has_value());
	UAM_ASSERT(uam::UamControlService::ResolveApproval(app, chat.id, pending->request_id_json, {{"userQuestion", {"No"}}}, &error));
	structured.chat_id = chat.id;
	terminal.uam_control_session->active_uam_agent_workspace_access = "read";
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(app, capability_id, {{"requestId", "read-only-goal"}, {"method", "goal_create"}, {"arguments", {{"objective", "Write changes"}, {"idempotencyKey", "read-only"}}}}, uam::time::SystemEpochMillisecondsNow()).value("ok", true));
	terminal.running = false;
	terminal.uam_control_session.reset();
	UAM_ASSERT(!uam::UamControlService::HandleRequestForTests(app, capability_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", true));
	UAM_ASSERT(uam::UamControlService::HandleRequestForTests(app, structured_id, request, uam::time::SystemEpochMillisecondsNow()).value("ok", false));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(CentralSkillToolsReadInstalledAssetsAndKeepCustomAgentScope)
{
	TempDir temp("uam-central-skill-tools");
	fs::create_directories(temp.root / "skill" / "references");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/SKILL.md", "Read references/example.md"));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "skill/references/example.md", "Full supporting asset"));
	uam::AppState app;
	app.data_root = temp.root / "data";
	app.settings.central_provider_configuration.enabled = true;
	app.settings.central_provider_configuration.skill_directories = {(temp.root / "skill").string()};
	ChatSession chat;
	chat.id = "central-skill-tools";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = true;
	app.chats.push_back(chat);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(app.data_root, app.settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT(uam::UamControlService::Initialize(app, &error));
	app.acp_sessions.push_back(std::make_unique<uam::AcpSessionState>());
	uam::AcpSessionState& session = *app.acp_sessions.back();
	session.chat_id = chat.id;
	session.provider_id = chat.provider_id;
	session.provider_configuration_directory = bundle.local_directory;
	session.running = true;
	nlohmann::json setup = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
	UAM_ASSERT(uam::UamControlService::AppendSessionMcpServer(app, session, chat, "session/new", setup, &error));
	const std::string id = session.uam_control_capability_id;
	const std::int64_t now = uam::time::SystemEpochMillisecondsNow();
	const nlohmann::json listing = uam::UamControlService::HandleRequestForTests(app, id, {{"requestId", "list-skills"}, {"method", "skill_list"}, {"arguments", nlohmann::json::object()}}, now);
	UAM_ASSERT(listing.value("ok", false));
	UAM_ASSERT_EQ(listing["result"][0]["id"], nlohmann::json("skill"));
	const nlohmann::json read = {{"requestId", "read-asset"}, {"method", "skill_read"}, {"arguments", {{"id", "skill"}, {"path", "references/example.md"}}}};
	const nlohmann::json asset = uam::UamControlService::HandleRequestForTests(app, id, read, now);
	UAM_ASSERT(asset.value("ok", false));
	UAM_ASSERT_EQ(asset["result"]["body"], nlohmann::json("Full supporting asset"));
	app.uam_control_capabilities.back().central_skills_enabled = false;
	const nlohmann::json denied = uam::UamControlService::HandleRequestForTests(app, id, {{"requestId", "read-denied"}, {"method", "skill_read"}, {"arguments", {{"id", "skill"}}}}, now);
	UAM_ASSERT(!denied.value("ok", true));
	uam::UamControlService::Shutdown(app);
}

UAM_TEST(SavedMcpSettingsReachAllFiveRuntimesWithoutEnablingCentralInstructions)
{
	TempDir temp("uam-mcp-propagation");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "shared-http";
	server.name = "shared-http";
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	server.headers = {{"Authorization", "UAM_PROPAGATION_TEST_SECRET"}};
	settings.mcp_servers = {server};
	// The Settings store is the only source. No provider home files are written.
	UAM_ASSERT(SettingsStore::Save(temp.root / "settings.txt", settings));
	AppSettings loaded;
	UAM_ASSERT(SettingsStore::Load(temp.root / "settings.txt", loaded).loaded);
	UAM_ASSERT(!loaded.central_provider_configuration.enabled);
	ScopedEnvVar secret("UAM_PROPAGATION_TEST_SECRET", "target-secret");
	for (const std::string& host_id : {"local", "ssh-linux", "ssh-windows"})
	{
		ExecutionHost host;
		host.id = host_id;
		host.transport = host_id == "local" ? "local" : "ssh";
		ChatSession chat;
		chat.id = "mcp-propagation";
		chat.uam_agent_id = "missing-agent-must-not-block-mcp";
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / host_id, loaded, chat, host, temp.root, bundle, error));
		UAM_ASSERT(!bundle.local_directory.empty());
		UAM_ASSERT(bundle.manifest.value("mcpOnly", false));
		UAM_ASSERT(bundle.manifest["files"].empty());
		UAM_ASSERT(bundle.manifest.dump().find("target-secret") == std::string::npos);
		for (const std::string& provider : {"codex-cli", "claude-cli", "gemini-cli", "opencode-cli", "copilot-cli"})
		{
			std::vector<std::string> argv{"provider", "--append-system-prompt", "existing instructions"};
			uam::provider_setup::Environment environment;
			UAM_ASSERT(uam::provider_setup::Apply(provider, bundle.local_directory, temp.root, argv, environment, error));
			UAM_ASSERT(std::ranges::find(argv, "existing instructions") != argv.end());
			UAM_ASSERT(std::ranges::find(argv, "--append-system-prompt-file") == argv.end());
			if (provider == "codex-cli")
			{
				UAM_ASSERT(std::ranges::any_of(argv, [](const std::string& arg) { return arg.find("mcp_servers.") != std::string::npos && arg.find(".url=") != std::string::npos; }));
				UAM_ASSERT(std::ranges::none_of(argv, [](const std::string& arg) { return arg.find("target-secret") != std::string::npos || arg.starts_with("developer_instructions="); }));
			}
			else if (provider == "opencode-cli")
			{
				const nlohmann::json config = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
				UAM_ASSERT_EQ(config["mcp"]["shared-http"]["url"], nlohmann::json(server.url));
				UAM_ASSERT_EQ(config["mcp"]["shared-http"]["headers"]["Authorization"], nlohmann::json("target-secret"));
			}
			else
			{
				std::string configuration_path;
				if (provider == "gemini-cli") configuration_path = uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH");
				else
				{
					const auto flag = std::ranges::find(argv, provider == "claude-cli" ? "--mcp-config" : "--additional-mcp-config");
					UAM_ASSERT(flag != argv.end());
					configuration_path = *(flag + 1);
					if (configuration_path.starts_with("@")) configuration_path.erase(0, 1);
				}
				const nlohmann::json config = nlohmann::json::parse(ReadFile(uam::paths::PathFromUtf8(configuration_path)));
				const nlohmann::json native = config["mcpServers"]["shared-http"];
				UAM_ASSERT_EQ(native[provider == "gemini-cli" ? "httpUrl" : "url"], nlohmann::json(server.url));
				UAM_ASSERT_EQ(native["headers"]["Authorization"], nlohmann::json("target-secret"));
			}
		}
	}
}

UAM_TEST(SharedMcpFollowsSourceWorkspaceIntoAnIsolatedWorktree)
{
	TempDir temp("uam-mcp-worktree-scope");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "source-server";
	server.name = "source-server";
	server.workspace_directory = (temp.root / "source").string();
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	settings.mcp_servers = {server};
	ChatSession chat;
	chat.workspace_source_directory = server.workspace_directory;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root / "worktree", bundle, error));
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"].size(), std::size_t{1});
	UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["name"], nlohmann::json(server.name));
	McpServerConfiguration override = server;
	override.id = "worktree-override";
	override.workspace_directory = (temp.root / "worktree").string();
	override.url = "http://localhost:5001/mcp";
	for (const bool source_first : {false, true})
	{
		settings.mcp_servers = source_first ? std::vector<McpServerConfiguration>{server, override} : std::vector<McpServerConfiguration>{override, server};
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root / "worktree", bundle, error));
		UAM_ASSERT_EQ(bundle.manifest["mcpServers"][0]["url"], nlohmann::json(override.url));
	}
	bundle.manifest["mcpOnly"] = "yes";
	UAM_ASSERT(!uam::provider_setup::ValidateManifest(bundle.manifest, error));
}

UAM_TEST(McpOnlySetupPreservesGeminiContextConfiguration)
{
	TempDir temp("uam-mcp-gemini-preservation");
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "shared-http";
	server.name = "shared-http";
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	settings.mcp_servers = {server};
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	const nlohmann::json original = {{"context", {{"fileName", "CUSTOM.md"}, {"loadMemoryFromIncludeDirectories", false}}}};
	const fs::path system_settings = temp.root / "system-settings.json";
	UAM_ASSERT(uam::io::WriteTextFile(system_settings, original.dump()));
	std::vector<std::string> argv{"gemini"};
	uam::provider_setup::Environment environment{{"GEMINI_CLI_SYSTEM_SETTINGS_PATH", system_settings.string()}};
	UAM_ASSERT(uam::provider_setup::Apply("gemini-cli", bundle.local_directory, temp.root, argv, environment, error));
	const nlohmann::json generated = nlohmann::json::parse(ReadFile(uam::paths::PathFromUtf8(uam::provider_setup::EnvironmentValue(environment, "GEMINI_CLI_SYSTEM_SETTINGS_PATH"))));
	UAM_ASSERT_EQ(generated["context"], original["context"]);
	UAM_ASSERT_EQ(generated["mcpServers"][server.name]["httpUrl"], nlohmann::json(server.url));
	UAM_ASSERT_EQ(argv, std::vector<std::string>{"gemini"});
}

UAM_TEST(CliViewPreparesMcpInheritedFromSourceWorkspace)
{
	TempDir temp("uam-mcp-cli-source");
	uam::AppState app;
	app.data_root = temp.root / "data";
	ChatSession chat;
	chat.id = "source-mcp-cli";
	chat.provider_id = "opencode-cli";
	chat.uam_control_enabled = false;
	chat.workspace_directory = (temp.root / "worktree").string();
	chat.workspace_source_directory = (temp.root / "source").string();
	fs::create_directories(chat.workspace_directory);
	McpServerConfiguration server;
	server.id = "source-mcp";
	server.name = "source-mcp";
	server.workspace_directory = chat.workspace_source_directory;
	server.transport = "http";
	server.url = "http://localhost:5000/mcp";
	app.settings.mcp_servers = {server};
	ExecutionHost host;
	uam::CliTerminalState terminal;
	std::vector<std::string> argv{"opencode"};
	uam::provider_setup::Environment environment;
	std::string channel;
	std::string error;
	UAM_ASSERT(!uam::PrepareCliProviderHandoffAsync(app, terminal, chat, host, argv, environment, channel, error));
	UAM_ASSERT(error.empty());
	UAM_ASSERT(terminal.context_preparation != nullptr);
	app.cli_context_preparation_tasks.back().worker->join();
	UAM_ASSERT(uam::PrepareCliProviderHandoffAsync(app, terminal, chat, host, argv, environment, channel, error));
	const nlohmann::json generated = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
	UAM_ASSERT_EQ(generated["mcp"][server.name]["url"], nlohmann::json(server.url));
}

UAM_TEST(McpWindowsWorkspaceScopeMatchesAcrossControllerPlatforms)
{
	McpServerConfiguration global;
	global.id = "global";
	global.name = "shared";
	global.transport = "http";
	global.url = "http://localhost:5000/mcp";
	McpServerConfiguration scoped = global;
	scoped.id = "windows-workspace";
	scoped.execution_host_id = "ssh-windows";
	scoped.workspace_directory = R"(C:\Projects\Example)";
	scoped.url = "http://localhost:5001/mcp";
	std::vector<McpServerConfiguration> servers{global, scoped};
	std::string error;
	UAM_ASSERT(uam::mcp_server_config::NormalizeAndValidate(servers, &error));
	const std::vector<McpServerConfiguration> selected = uam::mcp_server_config::SelectForWorkspace(servers, "c:/projects/example/", "ssh-windows");
	UAM_ASSERT_EQ(selected.size(), std::size_t{1});
	UAM_ASSERT_EQ(selected.front().id, scoped.id);
	UAM_ASSERT_EQ(uam::mcp_server_config::SelectForWorkspace(servers, "C:/Projects/Other", "ssh-windows").front().id, global.id);
	UAM_ASSERT_EQ(uam::mcp_server_config::SelectForWorkspace(servers, "C:/Projects/Example", "local").front().id, global.id);
}

UAM_TEST(ConcurrentCentralPreparationPublishesCompleteBundles)
{
	TempDir temp("uam-central-concurrent");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = std::string(256 * 1024, 'x');
	ChatSession chat;
	ExecutionHost host;
	for (int round = 0; round < 4; ++round)
	{
		std::barrier start(12);
		std::vector<std::jthread> workers;
		std::vector<std::string> errors(12);
		for (int index = 0; index < 12; ++index)
			workers.emplace_back([&, index]
			{
				start.arrive_and_wait();
				uam::ProviderConfigurationBundle bundle;
				if (!uam::PrepareProviderConfiguration(temp.root / std::to_string(round), settings, chat, host, temp.root, bundle, errors[index])) return;
				std::vector<std::string> missing;
				if (!uam::provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, errors[index])) return;
				if (!missing.empty()) errors[index] = "Published bundle is incomplete.";
			});
		workers.clear();
		for (const std::string& error : errors) if (!error.empty()) throw std::runtime_error(error);
	}
}

UAM_TEST(CentralPreparationRecoversInterruptedOwnershipPublication)
{
	TempDir temp("uam-central-interrupted-owner");
	const nlohmann::json manifest = {{"format", 1}, {"files", nlohmann::json::object()}};
	const fs::path directory = temp.root / uam::provider_setup::Digest(manifest.dump());
	fs::create_directories(directory);
	std::vector<std::string> missing;
	std::string error;
	UAM_ASSERT(uam::provider_setup::Prepare(directory, manifest, missing, error));
	UAM_ASSERT_EQ(ReadFile(directory / "owner.json"), manifest.dump());
}

UAM_TEST(CentralPreparationRecoversStagingFilesAndRejectsUnownedData)
{
	TempDir temp("uam-central-staging-recovery");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Shared resource";
	ChatSession chat;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	const fs::path target = temp.root / "interrupted" / bundle.local_directory.filename();
	fs::create_directories(target);
	UAM_ASSERT(uam::io::WriteTextFile(target / "context-settings-123.json", "unfinished"));
	std::vector<std::string> missing;
	UAM_ASSERT(uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT_EQ(missing.size(), bundle.files.size());
	for (const auto& [name, bytes] : bundle.files)
	{
		fs::create_directories((target / name).parent_path());
		UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target / name, bytes, error));
	}
	UAM_ASSERT(uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT(missing.empty());
	const fs::path foreign = temp.root / "foreign" / bundle.local_directory.filename();
	fs::create_directories(foreign);
	UAM_ASSERT(uam::io::WriteTextFile(foreign / "user.txt", "keep"));
	UAM_ASSERT(!uam::provider_setup::Prepare(foreign, bundle.manifest, missing, error));
	UAM_ASSERT(!fs::exists(foreign / "owner.json"));
	UAM_ASSERT_EQ(ReadFile(foreign / "user.txt"), std::string("keep"));
	UAM_ASSERT(uam::io::WriteTextFile(target / "owner.json", "other owner"));
	UAM_ASSERT(!uam::provider_setup::Prepare(target, bundle.manifest, missing, error));
	UAM_ASSERT_EQ(ReadFile(target / "owner.json"), std::string("other owner"));
}

UAM_TEST(ImmutableCentralPublicationKeepsExistingData)
{
	TempDir temp("uam-central-immutable-publication");
	const fs::path target = temp.root / "resource.md";
	std::string error;
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "published", error, false));
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "published", error, false));
	UAM_ASSERT(!uam::provider_setup::WriteRuntimeFile(target, "different", error, false));
	UAM_ASSERT_EQ(ReadFile(target), std::string("published"));
	UAM_ASSERT(uam::provider_setup::WriteRuntimeFile(target, "updated runtime setting", error));
	UAM_ASSERT_EQ(ReadFile(target), std::string("updated runtime setting"));
}

UAM_TEST(CentralNativeSetupInstallsTheQueuedAgentSnapshot)
{
	TempDir temp("uam-central-native-snapshot");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "SHARED_NATIVE_INSTRUCTIONS";
	ChatSession chat;
	chat.uam_agent_id = "build";
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	const std::string snapshot = "QUEUED_NATIVE_AGENT_SNAPSHOT";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error, &snapshot));
	UAM_ASSERT(bundle.files.at("AGENTS.md").find(snapshot) != std::string::npos);
	UAM_ASSERT(bundle.files.at("AGENTS.md").find("SHARED_NATIVE_INSTRUCTIONS") != std::string::npos);
	UAM_ASSERT_EQ(bundle.files.at("agents/build.md"), snapshot);
	UAM_ASSERT_EQ(ReadFile(bundle.local_directory / "agents/build.md"), snapshot);
}

UAM_TEST(CentralNativeSetupFindsSelectedAgentInSourceWorkspace)
{
	TempDir temp("uam-central-source-agent-snapshot");
	const fs::path source = temp.root / "source";
	const fs::path worktree = temp.root / "worktree";
	fs::create_directories(source / ".uam" / "agents");
	fs::create_directories(worktree);
	const std::string agent_markdown = "---\nversion: 1\nname: workspace-agent\ndescription: Parent workspace agent\nmode: primary\nworkspaceAccess: read\nskills: []\ndelegates: []\n---\nParent workspace agent.";
	UAM_ASSERT(uam::io::WriteTextFile(source / ".uam/agents/workspace-agent.md", agent_markdown));
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	ChatSession chat;
	chat.uam_agent_id = "workspace-agent";
	chat.workspace_source_directory = uam::paths::Utf8PathString(source);
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	const std::string queued_snapshot = "PINNED_PARENT_AGENT_SNAPSHOT";
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, worktree, bundle, error, &queued_snapshot));
	UAM_ASSERT_EQ(bundle.files.at("agents/workspace-agent.md"), queued_snapshot);
	UAM_ASSERT(bundle.files.at("AGENTS.md").find(queued_snapshot) != std::string::npos);
}

UAM_TEST(McpServersAcceptLanUrlsAndLiteralValuesButRejectCredentialUrls)
{
	McpServerConfiguration remote{.id = "penpot", .name = "penpot", .transport = "http", .url = "http://main.homelab.com:9001/mcp/stream?userToken=abc"};
	remote.headers = {{"Authorization", "", "Bearer token"}};
	TempDir temp("uam-mcp-literal-values");
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "server", "#!/bin/sh\n"));
	McpServerConfiguration local{.id = "searxng", .name = "searxng", .transport = "stdio", .command = uam::paths::Utf8PathString(temp.root / "server")};
	local.environment = {{"SEARXNG_URL", "", "http://main.homelab.com:8081"}};
	std::vector<McpServerConfiguration> servers{remote, local};
	std::string error;
	UAM_ASSERT(uam::mcp_server_config::NormalizeAndValidate(servers, &error));
	const std::vector<McpServerConfiguration> round_trip = uam::mcp_server_config::Parse(uam::mcp_server_config::Serialize(servers));
	UAM_ASSERT_EQ(round_trip[1].environment.front().value, std::string("http://main.homelab.com:8081"));
	const nlohmann::json resolved = uam::mcp_server_config::ResolveForWorkspace(round_trip, "/any", true, true, &error);
	UAM_ASSERT(resolved.is_array() && resolved.size() == 2);
	UAM_ASSERT_EQ(resolved[0]["headers"][0]["value"].get<std::string>(), std::string("Bearer token"));
	UAM_ASSERT_EQ(resolved[1]["env"][0]["value"].get<std::string>(), std::string("http://main.homelab.com:8081"));
	std::vector<McpServerConfiguration> credentialed{remote};
	credentialed.front().url = "http://user:pass@main.homelab.com/mcp";
	UAM_ASSERT(!uam::mcp_server_config::NormalizeAndValidate(credentialed, &error));
	std::vector<McpServerConfiguration> both{local};
	both.front().environment.front().environment_variable = "SEARXNG_URL";
	UAM_ASSERT(!uam::mcp_server_config::NormalizeAndValidate(both, &error));
}

UAM_TEST(CodexCentralAppServerSetupPreservesLargeInstructionsWithoutLaunchArguments)
{
	TempDir temp("uam-codex-large-central-instructions");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Quoted \"instruction\"\n" + std::string(192 * 1024, 'x') + "\nEnd marker";
	ChatSession chat;
	chat.id = "large-codex";
	chat.provider_id = uam::provider_ids::kCodexCli;
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, temp.root, bundle, error));
	std::vector<std::string> argv{"codex", "app-server", "--listen", "stdio://"};
	uam::provider_setup::Environment environment;
	std::string instructions;
	UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error, nlohmann::json::array(), &instructions));
	UAM_ASSERT(instructions.find(settings.central_provider_configuration.instructions) != std::string::npos);
	UAM_ASSERT(std::ranges::none_of(argv, [](const std::string& argument) { return argument.starts_with("developer_instructions=") || argument.size() > 12 * 1024; }));
}

UAM_TEST(CodexSetupInputPreservesFragmentedFramesAndRejectsInvalidSetup)
{
	const std::string instructions = "Complete \"quoted\" \n UTF8: caf\xc3\xa9 " + std::string(192 * 1024, 'x');
	for (const std::string& method : {"thread/start", "thread/resume"})
	{
		const std::string initialize = R"({"id":1,"method":"initialize","params":{}})" "\n";
		const nlohmann::json setup = {{"id", 2}, {"method", method}, {"params", {{"cwd", "workspace"}, {"threadId", "saved-thread"}}}};
		const std::string bytes = initialize + setup.dump() + "\n" + "opaque remaining prompt";
		uam::codex::SetupInputState state;
		std::string wire;
		for (const char byte : bytes)
		{
			std::string output, error;
			uam::codex::SetupInputState next;
			UAM_ASSERT(uam::codex::PrepareSetupInput(instructions, state, std::string_view(&byte, 1), output, next, error));
			state = std::move(next);
			wire += output;
		}
		UAM_ASSERT(state.configured && state.pending.empty());
		UAM_ASSERT(wire.starts_with(initialize));
		const std::size_t end = wire.find('\n', initialize.size());
		const nlohmann::json delivered = nlohmann::json::parse(wire.substr(initialize.size(), end - initialize.size()));
		UAM_ASSERT_EQ(delivered["params"]["developerInstructions"], nlohmann::json(instructions));
		UAM_ASSERT_EQ(delivered["params"]["threadId"], setup["params"]["threadId"]);
		UAM_ASSERT_EQ(wire.substr(end + 1), std::string("opaque remaining prompt"));
		std::string output, error;
		uam::codex::SetupInputState next;
		UAM_ASSERT(uam::codex::PrepareSetupInput(instructions, {}, bytes, output, next, error));
		UAM_ASSERT_EQ(output, wire);
	}
	for (const std::string& invalid : {std::string("not-json\n"), std::string(R"({"method":"thread/start","params":null})" "\n"), std::string(32 * 1024 * 1024 + 1, 'x')})
	{
		uam::codex::SetupInputState state, next;
		std::string output, error;
		UAM_ASSERT(!uam::codex::PrepareSetupInput(instructions, state, invalid, output, next, error));
		UAM_ASSERT(!error.empty() && state.pending.empty() && !state.configured);
	}
	uam::codex::SetupInputState next;
	std::string output, error;
	UAM_ASSERT(uam::codex::PrepareSetupInput("", {}, "opaque\n", output, next, error));
	UAM_ASSERT_EQ(output, std::string("opaque\n"));
}

UAM_TEST(RemoteCodexCentralSetupUsesCompleteInstructionsAndIdempotentInput)
{
	TempDir temp("uam-remote-codex-setup-wire");
	const fs::path workspace = fs::weakly_canonical(temp.root);
	const std::string instructions = "Complete central marker\n" + std::string(192 * 1024, 'x');
	const nlohmann::json manifest = {{"format", 1}, {"files", {{"AGENTS.md", {{"size", instructions.size()}, {"digest", uam::provider_setup::Digest(instructions)}}}}}};
	const fs::path directory = workspace / ".UAM/provider-setup" / uam::provider_setup::Digest(manifest.dump());
	uam::remote::RunnerState runner;
	UAM_ASSERT(runner.HandleProcessRequest({{"id", "prepare"}, {"type", "configuration.prepare"}, {"directory", directory.string()}, {"manifest", manifest}}).value("ok", false));
	UAM_ASSERT(uam::io::WriteTextFile(directory / "AGENTS.md", instructions));
	const auto request = [&](nlohmann::json body)
	{
		body["id"] = "request";
		body["sessionId"] = "codex-setup-wire";
		body["controlToken"] = "owned-codex-control";
		return runner.HandleProcessRequest(body);
	};
	UAM_ASSERT(request({{"type", "process.start"}, {"cwd", workspace.string()}, {"argv", nlohmann::json::array({uam::paths::Utf8PathString(PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath()), "--uam-test-codex-setup", "app-server"})}, {"configurationDirectory", directory.string()}, {"configurationProvider", "codex-cli"}}).value("ok", false));
	const std::string first = R"({"id":1,"method":"thread/resume","params":{"threadId":")";
	const std::string second = "saved-native\"}}\n";
	const auto write = [&](const std::string& bytes, std::uint64_t sequence)
	{
		return request({{"type", "process.write"}, {"dataBase64", uam::base64::Encode(bytes)}, {"inputSequence", sequence}, {"deliveryId", "owned-" + std::to_string(sequence)}});
	};
	UAM_ASSERT(write(first, 1).value("ok", false));
	UAM_ASSERT(write(first, 1)["result"].value("duplicate", false));
	UAM_ASSERT(!request({{"type", "process.closeInput"}}).value("ok", true));
	UAM_ASSERT(!write("bad\n", 2).value("ok", true));
	UAM_ASSERT(write(second, 2).value("ok", false));
	UAM_ASSERT(write(second, 2)["result"].value("duplicate", false));
	UAM_ASSERT(request({{"type", "process.closeInput"}}).value("ok", false));
	std::string output;
	bool exited = false;
	for (int attempt = 0; attempt < 300; ++attempt)
	{
		const nlohmann::json poll = request({{"type", "process.poll"}});
		UAM_ASSERT(poll.value("ok", false));
		std::string chunk;
		UAM_ASSERT(uam::base64::Decode(poll["result"].value("stdoutBase64", ""), chunk));
		output += chunk;
		if (!poll["result"].value("running", true)) { exited = true; UAM_ASSERT_EQ(poll["result"].value("exitCode", -1), 0); break; }
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	UAM_ASSERT(exited);
	const nlohmann::json delivered = nlohmann::json::parse(output);
	UAM_ASSERT_EQ(delivered["params"]["developerInstructions"], nlohmann::json(instructions));
	UAM_ASSERT_EQ(delivered["params"]["threadId"], nlohmann::json("saved-native"));
	UAM_ASSERT(request({{"type", "process.remove"}}).value("ok", false));
}

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(CentralInstructionFilesAuthorizeSavedPathsAndPreserveBytes)
{
	TempDir temp("uam-central-file-edit");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string original = "\xef\xbb\xbf# Shared\r\nKeep exact bytes.\r\n";
	UAM_ASSERT(uam::io::WriteTextFile(path, original));
	const fs::file_time_type old_modified = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(path, old_modified);
	CentralProviderConfiguration configuration;
	const std::string requested = uam::paths::Utf8PathString(path);
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
	configuration.instruction_files = {requested};
	const auto loaded = uam::central_instruction_files::Read(configuration, requested);
	UAM_ASSERT(loaded.ok);
	UAM_ASSERT_EQ(loaded.content, original);
	const std::string changed = original + "More rules.\r\n";
	const auto prepared = uam::central_instruction_files::PrepareSave(configuration, requested, changed, original);
	UAM_ASSERT(prepared->result.ok);
	UAM_ASSERT_EQ(ReadFile(path), original);
	const auto saved = uam::central_instruction_files::CommitSave(configuration, *prepared);
	UAM_ASSERT(saved.ok);
	UAM_ASSERT_EQ(saved.content, changed);
	UAM_ASSERT_EQ(ReadFile(path), changed);
	UAM_ASSERT(fs::last_write_time(path) > old_modified);
	const fs::file_time_type saved_modified = fs::last_write_time(path);
	const auto retry = uam::central_instruction_files::PrepareSave(configuration, requested, changed, original);
	UAM_ASSERT(retry->result.ok);
	UAM_ASSERT(retry->temporary.empty());
	UAM_ASSERT(uam::central_instruction_files::CommitSave(configuration, *retry).ok);
	UAM_ASSERT_EQ(fs::last_write_time(path), saved_modified);
	const auto empty = uam::central_instruction_files::PrepareSave(configuration, requested, "", changed);
	UAM_ASSERT(empty->result.ok);
	UAM_ASSERT(uam::central_instruction_files::CommitSave(configuration, *empty).ok);
	UAM_ASSERT_EQ(ReadFile(path), std::string{});
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(CentralInstructionFilesRejectStaleContentAndConfiguration)
{
	TempDir temp("uam-central-file-stale");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	CentralProviderConfiguration configuration;
	configuration.instruction_files = {requested};
	UAM_ASSERT(!uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "stale")->result.ok);
	const auto prepared = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original");
	UAM_ASSERT(prepared->result.ok);
	CentralProviderConfiguration removed;
	UAM_ASSERT(!uam::central_instruction_files::CommitSave(removed, *prepared).ok);
	UAM_ASSERT_EQ(ReadFile(path), std::string("original"));
	UAM_ASSERT(uam::io::WriteTextFile(path, "external"));
	UAM_ASSERT(!uam::central_instruction_files::CommitSave(configuration, *prepared).ok);
	UAM_ASSERT_EQ(ReadFile(path), std::string("external"));
}
#endif

UAM_TEST(CentralInstructionFilesRejectUnsafeTextAndFiles)
{
	TempDir temp("uam-central-file-validation");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	CentralProviderConfiguration configuration;
	configuration.instruction_files = {requested};
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
	for (const std::string& text : {std::string("a\0b", 3), std::string("\xc0\xaf", 2), std::string(2 * 1024 * 1024 + 1, 'x')})
	{
		UAM_ASSERT(uam::io::WriteTextFile(path, text));
		UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
		UAM_ASSERT(uam::io::WriteTextFile(path, "safe"));
		UAM_ASSERT(!uam::central_instruction_files::PrepareSave(configuration, requested, text, "safe")->result.ok);
		UAM_ASSERT_EQ(ReadFile(path), std::string("safe"));
	}
	fs::remove(path);
	fs::create_directory(path);
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
	fs::remove(path);
#if !defined(_WIN32)
	const fs::path outside = temp.root / "outside.md";
	UAM_ASSERT(uam::io::WriteTextFile(outside, "outside"));
	fs::create_symlink(outside, path);
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
	UAM_ASSERT(!uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "outside")->result.ok);
	UAM_ASSERT_EQ(ReadFile(outside), std::string("outside"));
	fs::remove(path);
	UAM_ASSERT(mkfifo(path.c_str(), 0600) == 0);
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, requested).ok);
#endif
}

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(CentralInstructionFilesStageFailureAndMetadataStaySafe)
{
	TempDir temp("uam-central-file-durable");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	CentralProviderConfiguration configuration;
	configuration.instruction_files = {requested};
	const auto failed = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original",
	    [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::DuringWrite; });
	UAM_ASSERT(!failed->result.ok);
	UAM_ASSERT_EQ(ReadFile(path), std::string("original"));
#if defined(__APPLE__)
	UAM_ASSERT(chmod(path.c_str(), 0640) == 0);
	const auto prepared = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original");
	UAM_ASSERT(prepared->result.ok);
	UAM_ASSERT(uam::central_instruction_files::CommitSave(configuration, *prepared).ok);
	struct stat info{};
	UAM_ASSERT(stat(path.c_str(), &info) == 0);
	UAM_ASSERT_EQ(info.st_mode & 0777, 0640);
#endif
}
#endif

#if defined(__APPLE__) || defined(_WIN32)
UAM_TEST(CentralInstructionFilesRecheckIdentityAndRejectParentLinks)
{
	TempDir temp("uam-central-file-races");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	CentralProviderConfiguration configuration;
	configuration.instruction_files = {requested};
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	const auto prepared = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original");
	UAM_ASSERT(prepared->result.ok);
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	UAM_ASSERT(!uam::central_instruction_files::CommitSave(configuration, *prepared).ok);
	UAM_ASSERT_EQ(ReadFile(path), std::string("original"));
	fs::path abandoned;
	{
		const auto cancelled = uam::central_instruction_files::PrepareSave(configuration, requested, "cancelled", "original");
		UAM_ASSERT(cancelled->result.ok);
		abandoned = cancelled->temporary;
		UAM_ASSERT(fs::exists(abandoned));
	}
	UAM_ASSERT(!fs::exists(abandoned));
#if !defined(_WIN32)
	const fs::path linked = temp.root / "linked";
	fs::create_directory_symlink(temp.root, linked);
	const std::string alias = uam::paths::Utf8PathString(linked / "AGENTS.md");
	configuration.instruction_files.push_back(alias);
	UAM_ASSERT(!uam::central_instruction_files::Read(configuration, alias).ok);
	const auto before_link = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original");
	UAM_ASSERT(before_link->result.ok);
	const fs::path outside = temp.root / "outside.md";
	UAM_ASSERT(uam::io::WriteTextFile(outside, "outside"));
	fs::remove(path);
	fs::create_symlink(outside, path);
	UAM_ASSERT(!uam::central_instruction_files::CommitSave(configuration, *before_link).ok);
	UAM_ASSERT_EQ(ReadFile(outside), std::string("outside"));
#endif
}
#endif

UAM_TEST(CentralInstructionFilesAcceptBoundedEscapedBridgePayload)
{
	TempDir temp("uam-central-file-cap");
	const fs::path path = temp.root / "AGENTS.md";
	const std::string requested = uam::paths::Utf8PathString(path);
	const std::string content(2 * 1024 * 1024, '\x01');
	UAM_ASSERT(uam::io::WriteTextFile(path, content));
	CentralProviderConfiguration configuration;
	configuration.instruction_files = {requested};
	UAM_ASSERT_EQ(uam::central_instruction_files::Read(configuration, requested).content, content);
	const nlohmann::json request = {{"action", "saveCentralInstructionFile"}, {"payload", {{"path", requested}, {"content", content}, {"expectedContent", content}}}};
	const auto parsed = uam::cef::ParseBridgeRequest(request.dump());
	UAM_ASSERT(parsed.ok);
	UAM_ASSERT_EQ(parsed.request.payload.at("content").get<std::string>(), content);
	UAM_ASSERT_EQ(parsed.request.payload.at("expectedContent").get<std::string>(), content);
}

#if defined(__APPLE__)
UAM_TEST(CentralInstructionFilesPreserveAclAndExtendedAttributes)
{
	TempDir temp("uam-central-file-metadata");
	const fs::path path = temp.root / "AGENTS.md";
	UAM_ASSERT(uam::io::WriteTextFile(path, "original"));
	UAM_ASSERT(setxattr(path.c_str(), "com.uam.instruction-test", "keep", 4, 0, 0) == 0);
	acl_t acl = acl_init(1);
	UAM_ASSERT(acl != nullptr);
	acl_entry_t entry;
	UAM_ASSERT(acl_create_entry(&acl, &entry) == 0);
	UAM_ASSERT(acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0);
	uuid_t identifier;
	UAM_ASSERT(mbr_uid_to_uuid(getuid(), identifier) == 0);
	UAM_ASSERT(acl_set_qualifier(entry, identifier) == 0);
	acl_permset_t permissions;
	UAM_ASSERT(acl_get_permset(entry, &permissions) == 0);
	UAM_ASSERT(acl_add_perm(permissions, ACL_READ_DATA) == 0);
	UAM_ASSERT(acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl) == 0);
	CentralProviderConfiguration configuration;
	const std::string requested = uam::paths::Utf8PathString(path);
	configuration.instruction_files = {requested};
	const auto prepared = uam::central_instruction_files::PrepareSave(configuration, requested, "mine", "original");
	UAM_ASSERT(prepared->result.ok);
	UAM_ASSERT(uam::central_instruction_files::CommitSave(configuration, *prepared).ok);
	acl_t after = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
	UAM_ASSERT(after != nullptr);
	ssize_t before_length = 0; ssize_t after_length = 0;
	char* before_text = acl_to_text(acl, &before_length);
	char* after_text = acl_to_text(after, &after_length);
	UAM_ASSERT(before_text != nullptr && after_text != nullptr);
	UAM_ASSERT_EQ(std::string(before_text, before_length), std::string(after_text, after_length));
	acl_free(before_text); acl_free(after_text); acl_free(after); acl_free(acl);
	char attribute[4]{};
	UAM_ASSERT_EQ(getxattr(path.c_str(), "com.uam.instruction-test", attribute, sizeof(attribute), 0, 0), ssize_t(4));
	UAM_ASSERT_EQ(std::string(attribute, sizeof(attribute)), std::string("keep"));
}
#endif
