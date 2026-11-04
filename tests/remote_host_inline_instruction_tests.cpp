#include "test_harness.h"
#include "app/provider_configuration_service.h"
#include "remote/host_context.h"

using namespace uam_test;

UAM_TEST(RemoteInlineInstructionsPreserveLegacyEmptyAndTextStates)
{
	ExecutionHost host;
	host.id = "inline-fixture";
	host.transport = "ssh";
	host.ssh_alias = "never-connect";
	host.platform = "linux";
	host.instruction_file = "/legacy/AGENTS.md";
	const nlohmann::json legacy = uam::execution_hosts::Serialize({host});
	UAM_ASSERT(!uam::execution_hosts::Parse(legacy).back().instruction_text);
	nlohmann::json absent = legacy;
	absent.back().erase("instructionText");
	UAM_ASSERT(!uam::execution_hosts::Parse(absent).back().instruction_text);
	for (const std::string text : {std::string{}, std::string("Quoted \"text\"\nUnicode: ✓\n  preserved  ")})
	{
		host.instruction_text = text;
		const ExecutionHost restored = uam::execution_hosts::Parse(uam::execution_hosts::Serialize({host})).back();
		UAM_ASSERT(restored.instruction_text.has_value());
		UAM_ASSERT_EQ(*restored.instruction_text, text);
		UAM_ASSERT_EQ(restored.instruction_file, std::string("/legacy/AGENTS.md"));
		std::string context;
		std::string error;
		UAM_ASSERT(uam::remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service, restored, "/workspace", 0, context, error));
		UAM_ASSERT_EQ(context, text.empty() ? std::string{} : "--- BEGIN HOST INSTRUCTIONS ---\n" + text + "\n--- END HOST INSTRUCTIONS ---\n\n");
	}
	ExecutionHost changed = host;
	changed.instruction_text = "changed";
	UAM_ASSERT(!uam::execution_hosts::SameConnection(host, changed));
}

UAM_TEST(RemoteInlineInstructionsRejectMalformedAndOversizedText)
{
	ExecutionHost host;
	host.id = "inline-fixture";
	host.transport = "ssh";
	host.ssh_alias = "never-connect";
	host.platform = "linux";
	host.instruction_text = std::string(uam::execution_hosts::kMaxInstructionTextBytes, 'x');
	std::string error;
	UAM_ASSERT(uam::execution_hosts::NormalizeRemote(host, &error));
	nlohmann::json serialized = uam::execution_hosts::Serialize({host});
	UAM_ASSERT_EQ(uam::execution_hosts::Parse(serialized).size(), std::size_t(2));
	for (const nlohmann::json& malformed : {nlohmann::json(1), nlohmann::json(false), nlohmann::json::object(), nlohmann::json(std::string(uam::execution_hosts::kMaxInstructionTextBytes + 1, 'x'))})
	{
		serialized.back()["instructionText"] = malformed;
		UAM_ASSERT_EQ(uam::execution_hosts::Parse(serialized).size(), std::size_t(1));
	}
	host.instruction_text->push_back('x');
	UAM_ASSERT(!uam::execution_hosts::NormalizeRemote(host, &error));
	std::string context;
	UAM_ASSERT(!uam::remote::ResolveHostContext(PlatformServicesFactory::Instance().process_service, host, "/workspace", 0, context, error));
}

UAM_TEST(RemoteInlineInstructionsReachRunnerWithoutLegacyFileReads)
{
	TempDir temp("uam-inline-host-instructions");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = "Controller instruction";
	ChatSession chat;
	chat.provider_id = uam::provider_ids::kCodexCli;
	ExecutionHost host;
	host.id = "ssh-inline";
	host.transport = "ssh";
	host.instruction_file = (temp.root / "missing-legacy.md").string();
	for (const std::string text : {std::string("Inline host marker\n"), std::string{}})
	{
		host.instruction_text = text;
		uam::ProviderConfigurationBundle bundle;
		std::string error;
		UAM_ASSERT(uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
		UAM_ASSERT_EQ(bundle.manifest["hostInstructionFile"].get<std::string>(), std::string{});
		UAM_ASSERT(!bundle.manifest.contains("hostInstructionText"));
		std::vector<std::string> argv{"codex", "app-server", "--listen", "stdio://"};
		uam::provider_setup::Environment environment;
		std::string instructions;
		UAM_ASSERT(uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, temp.root, argv, environment, error, nlohmann::json::array(), &instructions));
		UAM_ASSERT_EQ(instructions.find("Inline host marker") != std::string::npos, !text.empty());
		UAM_ASSERT(!fs::exists(temp.root / "missing-legacy.md"));
	}
}

UAM_TEST(RemoteInlineInstructionsBoundCombinedProviderPayload)
{
	TempDir temp("uam-inline-host-size");
	AppSettings settings;
	settings.central_provider_configuration.enabled = true;
	settings.central_provider_configuration.instructions = std::string(uam::provider_setup::kMaxFileBytes - 1024, 'g');
	ChatSession chat;
	ExecutionHost host;
	host.id = "ssh-inline";
	host.transport = "ssh";
	host.instruction_text = std::string(2048, 'h');
	uam::ProviderConfigurationBundle bundle;
	std::string error;
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(error, std::string("Combined provider instructions exceed 2 MiB."));
	settings.central_provider_configuration.instructions.clear();
	host.instruction_text = std::string(uam::execution_hosts::kMaxInstructionTextBytes + 1, 'h');
	UAM_ASSERT(!uam::PrepareProviderConfiguration(temp.root / "controller", settings, chat, host, temp.root, bundle, error));
	UAM_ASSERT_EQ(error, std::string("Host instruction text exceeds 256 KiB."));
}
