#include "app/provider_configuration_service.h"
#include "app/provider_mcp_settings_service.h"
#include "common/config/settings_store.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct PrivateFiles
	{
		std::filesystem::path root;
		PrivateFiles()
		{
			root = std::filesystem::temp_directory_path() / ("uam-progressive-mcp-" + uam::provider_setup::Digest(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())));
			Require(std::filesystem::create_directory(root), "Create exclusive private fixture directory");
		}
		~PrivateFiles() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
	};

	struct CodeModeEnvironment
	{
		std::optional<std::string> prior;
		CodeModeEnvironment()
		{
			if (const char* value = std::getenv("OPENCODE_EXPERIMENTAL_CODE_MODE")) prior = value;
			Set(std::nullopt);
		}
		~CodeModeEnvironment() { Set(prior); }
		static void Set(const std::optional<std::string>& value)
		{
#if defined(_WIN32)
			_putenv_s("OPENCODE_EXPERIMENTAL_CODE_MODE", value ? value->c_str() : "");
#else
			if (value) setenv("OPENCODE_EXPERIMENTAL_CODE_MODE", value->c_str(), 1);
			else unsetenv("OPENCODE_EXPERIMENTAL_CODE_MODE");
#endif
		}
	};

	void SavedScopeAndCheckedVersionAdmission()
	{
		CentralProviderConfiguration configuration;
		Require(uam::central_configuration::OpenCodeProgressiveMcpEnabled(configuration, "local"), "Unset preference preserves enabled V1 default");
		configuration.mcp_overrides = {{"opencode-cli", "local", "opencode-progressive-mcp", false, true}, {"opencode-cli", "ssh-a", "opencode-progressive-mcp", true, true}};
		for (const std::string provider : {"opencode-cli", "opencode", "codex-cli", "gemini-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
			for (const std::string host : {"local", "ssh-a", "ssh-b"})
				for (const std::string version : {"", "1.17.19", "1.17.20", "1.18.35", "1.18.36", "1.19.4", "2.1.2"})
				{
					const bool expected = (provider == "opencode-cli" || provider == "opencode") && host != "local" && (version == "1.17.20" || version == "1.18.35");
					Require(uam::ShouldEnableOpenCodeV1ProgressiveMcp(configuration, provider, host, version) == expected, "Exact provider/host/version admission");
				}
		Require(!uam::central_configuration::OpenCodeProgressiveMcpEnabled(configuration, ""), "Empty host resolves to local only");
		const std::string captured = uam::central_configuration::Serialize(configuration).dump();
		configuration.mcp_overrides.front().enabled = true;
		Require(captured != uam::central_configuration::Serialize(configuration).dump(), "Preference participates in central launch capture");
	}

	void DurablePreferenceAndRejectedPublication()
	{
		PrivateFiles files;
		AppSettings settings;
		settings.central_provider_configuration.instructions = "Retain shared resources";
		settings.central_provider_configuration.mcp_overrides = {{"codex-cli", "local", "docs", false}, {"opencode-cli", "ssh-a", "opencode-progressive-mcp", false, true}};
		ExecutionHost remote; remote.id = "ssh-a"; remote.transport = "ssh"; remote.ssh_alias = "private-fixture";
		settings.execution_hosts = {ExecutionHost{}, remote};
		const auto path = files.root / "settings.txt";
		Require(SettingsStore::Save(path, settings), "Save real native settings");
		std::string error;
		const auto save = [&]() { return SettingsStore::Save(path, settings); };
		for (const bool enabled : {true, false})
		{
			Require(uam::provider_mcp_settings::SetConfiguration(settings, "ssh-a", "opencode-cli", "opencode-progressive-mcp", enabled, true, save, error) == 0, "Publish each direction through the production setter");
			AppSettings restored;
			Require(SettingsStore::LoadPrimary(path, restored).loaded, "Read primary settings after save");
			Require(uam::central_configuration::Serialize(restored.central_provider_configuration) == uam::central_configuration::Serialize(settings.central_provider_configuration), "Keep host, provider, unrelated overrides and resources");
			Require(uam::central_configuration::OpenCodeProgressiveMcpEnabled(restored.central_provider_configuration, "ssh-a") == enabled, "Saved exact-host choice");
			Require(uam::central_configuration::OpenCodeProgressiveMcpEnabled(restored.central_provider_configuration, "local"), "Remote preference does not overwrite local");
		}
		SettingsMemoryWriteReceipt receipt;
		const auto failed_save = [&]() { return SettingsStore::Save(path, settings, &receipt, [](uam::io::AtomicWriteStage stage) { return stage != uam::io::AtomicWriteStage::BeforeReplace; }); };
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "ssh-a", "opencode-cli", "opencode-progressive-mcp", true, true, failed_save, error) == 500, "Reject injected pre-replace failure");
		Require(!uam::central_configuration::OpenCodeProgressiveMcpEnabled(settings.central_provider_configuration, "ssh-a"), "Failed save restores running preference");
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "ssh-a", "opencode-cli", "opencode-progressive-mcp", true, true, []() -> bool { throw std::runtime_error("Injected failure"); }, error) == 500, "Thrown save also rejects");
		Require(!uam::central_configuration::OpenCodeProgressiveMcpEnabled(settings.central_provider_configuration, "ssh-a"), "Thrown save restores running preference");
		for (const std::string provider : {"codex-cli", "gemini-cli", "claude-cli", "copilot-cli", "antigravity-cli"})
			Require(uam::provider_mcp_settings::SetConfiguration(settings, "ssh-a", provider, "opencode-progressive-mcp", true, true, save, error) == 400, "Other providers cannot save this preference");
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "missing-host", "opencode-cli", "opencode-progressive-mcp", true, true, save, error) == 404, "Unknown host cannot save this preference");
		AppSettings unchanged;
		Require(SettingsStore::LoadPrimary(path, unchanged).loaded, "Prior primary stays readable");
		Require(uam::central_configuration::Serialize(unchanged.central_provider_configuration) == uam::central_configuration::Serialize(settings.central_provider_configuration), "Failure retains previous durable preference");
	}

	void ServerIdentityDoesNotAliasPreference()
	{
		PrivateFiles files;
		AppSettings settings;
		McpServerConfiguration server;
		server.id = "opencode-progressive-mcp"; server.name = "Private collision fixture";
		server.transport = "http"; server.url = "http://127.0.0.1:31084/mcp";
		settings.mcp_servers.push_back(server);
		std::string error;
		const auto path = files.root / "settings.txt";
		const auto save = [&]() { return SettingsStore::Save(path, settings); };
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "local", "opencode-cli", server.id, false, false, save, error) == 0, "Save a real server override with the same identifier");
		Require(uam::central_configuration::OpenCodeProgressiveMcpEnabled(settings.central_provider_configuration, "local"), "Server override does not opt out of discovery");
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "local", "opencode-cli", server.id, false, true, save, error) == 0, "Save separate discovery preference");
		Require(uam::provider_mcp_settings::SetConfiguration(settings, "local", "opencode-cli", server.id, true, false, save, error) == 0, "Enable only the real server");
		AppSettings restored;
		Require(SettingsStore::LoadPrimary(path, restored).loaded, "Reload colliding identities");
		Require(restored.central_provider_configuration.mcp_overrides.size() == 2, "Both namespace entries are durable");
		Require(!uam::central_configuration::OpenCodeProgressiveMcpEnabled(restored.central_provider_configuration, "local"), "Server change retains disabled discovery");
		Require(uam::central_configuration::McpEnabled(restored.central_provider_configuration, "opencode-cli", "local", server.id, false), "Discovery change retains enabled server");
		Require(restored.mcp_servers.size() == 1 && restored.mcp_servers.front().id == server.id, "User server identity is retained");
		auto document = uam::central_configuration::Serialize(restored.central_provider_configuration);
		document["mcpOverrides"][0]["kind"] = "unknown";
		CentralProviderConfiguration candidate;
		Require(!uam::central_configuration::Parse(document, candidate, error), "Reject unknown setting namespaces");
	}

	std::filesystem::path InstallManifest(const std::filesystem::path& root, bool discovery)
	{
		nlohmann::json manifest = {{"format", 1}, {"mcpOnly", true}, {"files", nlohmann::json::object()}, {"mcpServers", nlohmann::json::array()}};
		if (discovery) manifest["openCodeCodeMode"] = true;
		const auto directory = root / uam::provider_setup::Digest(manifest.dump());
		std::vector<std::string> missing;
		std::string error;
		Require(uam::provider_setup::Prepare(directory, manifest, missing, error), "Install real format-1 manifest");
		Require(missing.empty(), "MCP-only fixture requires no resource files");
		return directory;
	}

	void NativeEnvironmentAndConfigurationPrecedence()
	{
		PrivateFiles files;
		CodeModeEnvironment host_environment;
		const auto enabled = InstallManifest(files.root, true);
		const auto disabled = InstallManifest(files.root, false);
		const std::string native = R"({"instructions":["existing.md"],"mcp":{"existing":{"type":"remote","url":"https://example.test/mcp","enabled":true}}})";
		const auto native_file = files.root / "opencode.json";
		Require(uam::io::WriteTextFile(native_file, native), "Write private native configuration sentinel");
		for (const bool discovery : {false, true})
		{
			std::vector<std::string> argv{"opencode", "acp"};
			uam::provider_setup::Environment environment{{"OPENCODE_CONFIG_CONTENT", native}};
			std::string error;
			Require(uam::provider_setup::Apply("opencode-cli", discovery ? enabled : disabled, files.root, argv, environment, error), "Apply real native configuration");
			Require(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_EXPERIMENTAL_CODE_MODE") == (discovery ? "true" : ""), "Only enabled manifest supplies child opt-in");
			const auto merged = nlohmann::json::parse(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_CONFIG_CONTENT"));
			Require(merged.at("mcp") == nlohmann::json::parse(native).at("mcp"), "Discovery preference does not disable/register MCP definitions");
			Require(merged.at("instructions") == nlohmann::json::parse(native).at("instructions"), "MCP-only setup preserves native instructions");
			for (const std::string value : {"true", "false", ""})
			{
				environment = {{"OPENCODE_CONFIG_CONTENT", native}, {"OPENCODE_EXPERIMENTAL_CODE_MODE", value}};
				Require(uam::provider_setup::Apply("opencode-cli", discovery ? enabled : disabled, files.root, argv, environment, error), "Apply with explicit child override");
				Require(uam::provider_setup::EnvironmentValue(environment, "OPENCODE_EXPERIMENTAL_CODE_MODE") == value, "Explicit true/false/empty child environment wins");
				CodeModeEnvironment::Set(value);
				environment = {{"OPENCODE_CONFIG_CONTENT", native}};
				Require(uam::provider_setup::Apply("opencode-cli", discovery ? enabled : disabled, files.root, argv, environment, error), "Apply with executing-host override");
#if defined(_WIN32)
				if (!value.empty())
#endif
				Require(std::ranges::none_of(environment, [](const auto& entry) { return entry.first == "OPENCODE_EXPERIMENTAL_CODE_MODE"; }), "Executing-host environment wins without mutation");
				CodeModeEnvironment::Set(std::nullopt);
			}
		}
		std::string preserved, error;
		Require(uam::provider_setup::ReadFile(native_file, preserved, error) && preserved == native, "Native user-owned configuration sentinel is unchanged");
	}
}

int main()
{
	try
	{
		SavedScopeAndCheckedVersionAdmission();
		std::cout << "PASS SavedScopeAndCheckedVersionAdmission\n";
		DurablePreferenceAndRejectedPublication();
		std::cout << "PASS DurablePreferenceAndRejectedPublication\n";
		ServerIdentityDoesNotAliasPreference();
		std::cout << "PASS ServerIdentityDoesNotAliasPreference\n";
		NativeEnvironmentAndConfigurationPrecedence();
		std::cout << "PASS NativeEnvironmentAndConfigurationPrecedence\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
