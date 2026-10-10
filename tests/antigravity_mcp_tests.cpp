#include "test_harness.h"
#include "app/provider_configuration_service.h"
#include "app/uam_control_service.h"

using namespace uam_test;

#if UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI
UAM_TEST(AntigravityManagedMcpKeepsWorkspaceBytesAndConcurrentSessionsIsolated)
{
	TempDir temp("uam-agy-managed-isolation");
	const std::filesystem::path workspace = temp.root / "workspace";
	std::filesystem::create_directories(workspace / ".agents");
	const std::string original = R"({"mcpServers":{"existing":{"command":"user-owned"}}})";
	UAM_ASSERT(uam::io::WriteTextFile(workspace / ".agents" / "mcp_config.json", original));
	AppSettings settings;
	McpServerConfiguration server;
	server.id = "http-resource";
	server.name = "HTTP resource";
	server.transport = "http";
	server.url = "https://example.invalid/mcp";
	settings.mcp_servers.push_back(server);
	ChatSession chat;
	chat.id = "agy-managed";
	chat.provider_id = "antigravity-cli";
	chat.workspace_directory = workspace.string();
	ExecutionHost host;
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "data", settings, chat, host, workspace, bundle, error));
	const auto apply = [&](const char* name)
	{
		std::vector<std::string> argv{"agy"};
		uam::provider_setup::Environment environment;
		const nlohmann::json servers = {{{"name", name}, {"command", "test-fixture"}, {"args", nlohmann::json::array()}, {"env", nlohmann::json::array()}}};
		if (!uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error, servers)) throw std::runtime_error(error);
		UAM_ASSERT_EQ(argv[1], std::string("--add-dir"));
		const std::filesystem::path isolated = uam::paths::PathFromUtf8(argv[2]);
		UAM_ASSERT(isolated.string().starts_with(bundle.local_directory.string()));
		const nlohmann::json config = nlohmann::json::parse(uam::io::ReadTextFile(isolated / ".agents" / "mcp_config.json"));
		UAM_ASSERT(config["mcpServers"].contains(name));
		UAM_ASSERT(config["mcpServers"]["HTTP resource"]["serverUrl"] == server.url);
		UAM_ASSERT(!config["mcpServers"]["HTTP resource"].contains("url"));
		UAM_ASSERT(environment.empty());
		return isolated;
	};
	const std::filesystem::path first = apply("session-first");
	const std::filesystem::path second = apply("session-second");
	UAM_ASSERT(first != second);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(workspace / ".agents" / "mcp_config.json"), original);
	UAM_ASSERT(uam::UamControlService::SupportsStructuredProtocol("antigravity-stream-json"));
#if !defined(_WIN32)
	const std::filesystem::path outside = temp.root / "outside";
	std::filesystem::create_directories(outside);
	std::filesystem::remove_all(first / ".agents");
	std::filesystem::create_directory_symlink(outside, first / ".agents");
	std::vector<std::string> argv{"agy"};
	uam::provider_setup::Environment environment;
	const nlohmann::json servers = {{{"name", "session-first"}, {"command", "test-fixture"}, {"args", nlohmann::json::array()}, {"env", nlohmann::json::array()}}};
	UAM_ASSERT(!uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error, servers));
	UAM_ASSERT(error.find("symbolic link") != std::string::npos);
	UAM_ASSERT(std::filesystem::is_empty(outside));
#endif
}
#endif
