#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace uam
{
	struct AgentDefinition
	{
		std::string id;
		std::string description;
		std::string mode;
		std::string workspace_access;
		std::vector<std::string> skills;
		std::vector<std::string> delegates;
		std::string instructions;
		std::string definition_hash;
		std::string markdown_snapshot;
		std::filesystem::path source_path;
		bool workspace_override = false;
		bool built_in = false;
	};

	struct AgentDefinitionCatalog
	{
		std::vector<AgentDefinition> definitions;
		std::vector<std::string> errors;
	};

	struct ProviderAgentImportPreview
	{
		std::string provider_id;
		std::filesystem::path source_path;
		std::string suggested_id;
		std::string description;
		std::string mode;
		std::string instructions;
		std::vector<std::string> security_fields;
		std::vector<std::string> ignored_fields;
		std::string error;
		bool supported = false;
	};

	struct ProviderAgentImportRequest
	{
		std::string provider_id;
		std::filesystem::path source_path;
		std::string canonical_id;
		std::string workspace_access;
		std::vector<std::string> skills;
		std::vector<std::string> delegates;
		bool workspace_scope = false;
		bool acknowledge_ignored_fields = false;
		bool replace_existing = false;
	};

	struct ProviderAgentRuntimeAdapter
	{
		std::string execution_capability = "uam-prompt-injected";
		std::filesystem::path directory;
		std::vector<std::string> launch_arguments;
		std::vector<std::pair<std::string, std::string>> launch_environment;
		bool inject_in_prompt = true;
	};

	class AgentDefinitionService
	{
	  public:
		static AgentDefinitionCatalog Load(const std::filesystem::path& data_root,
		                                   const std::filesystem::path& workspace_root);
		static ProviderAgentImportPreview PreviewProviderAgentImport(
		    const std::string& provider_id, const std::filesystem::path& source_path);
		static bool ImportProviderAgent(const std::filesystem::path& data_root,
		                                const std::filesystem::path& workspace_root,
		                                const ProviderAgentImportRequest& request,
		                                AgentDefinition* imported_out = nullptr,
		                                std::string* error_out = nullptr);
		static std::string ExecutionCapabilityForProvider(const std::string& provider_id);
		static bool PrepareRuntimeAdapter(const std::filesystem::path& data_root,
		                                  const std::string& chat_id,
		                                  const std::string& provider_id,
		                                  const std::string& agent_id,
		                                  const std::string& instructions,
		                                  ProviderAgentRuntimeAdapter* adapter_out,
		                                  std::string* error_out = nullptr);
	};
} // namespace uam
