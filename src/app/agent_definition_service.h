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
		std::string open_code_permission_json;
	};

	struct AgentDefinitionCatalog
	{
		std::vector<AgentDefinition> definitions;
		std::vector<std::string> errors;
	};

	/** Worker-validated primary definition and the source revision required for admission. */
	struct SelectedPrimaryAgentSnapshot
	{
		AgentDefinition definition;
		std::filesystem::path data_root;
		std::filesystem::path workspace_root;
		std::string scope_revision;
	};

	struct ProviderAgentImportPreview
	{
		std::string provider_id;
		std::filesystem::path source_path;
		/// Agent key inside an OpenCode JSON(C) config; empty for a Markdown agent file.
		std::string source_entry;
		std::string suggested_id;
		std::string description;
		std::string mode;
		std::string instructions;
		std::vector<std::string> security_fields;
		std::vector<std::string> ignored_fields;
		/// Visible preview lines explaining each mapped restriction, shortened text or runtime limit.
		std::vector<std::string> notes;
		std::string error;
		bool supported = false;
		/// Normalised OpenCode rules (tool -> ask|deny) before the chosen access ceiling is applied.
		std::string open_code_permission_json;
		/// True when the rules hold a deny that workspace access alone cannot express, so the agent is bound to OpenCode.
		bool provider_bound = false;
		/// Highest workspace access the source permits; a read-only source never imports as write.
		std::string max_workspace_access = "write";
		std::string suggested_workspace_access = "read";
		/// The source field that makes the agent read-only, named in the access error.
		std::string read_only_reason;
	};

	struct ProviderAgentCandidate
	{
		std::string provider_id;
		std::string id;
		std::string description;
		std::filesystem::path source_path;
		std::string source_entry;
		std::string scope; // "global" or "workspace"
		bool already_imported = false;
	};

	struct ProviderAgentDiscovery
	{
		std::vector<ProviderAgentCandidate> candidates;
		std::vector<std::filesystem::path> searched;
		std::vector<std::string> errors;
		/// Set when the provider has no native agent files UAM can import.
		std::string unsupported;
	};

	struct ProviderAgentImportRequest
	{
		std::string provider_id;
		std::filesystem::path source_path;
		std::string source_entry;
		/// Locally checked OpenCode version; a 2.x version refuses provider-bound rules at preview.
		std::string local_opencode_version;
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
		static bool CaptureSelectedPrimary(const std::filesystem::path& data_root,
		    const std::filesystem::path& workspace_root, const std::string& agent_id,
		    SelectedPrimaryAgentSnapshot& snapshot, std::string& error);
		static bool SelectedPrimaryIsCurrent(const SelectedPrimaryAgentSnapshot& snapshot);
		static ProviderAgentImportPreview PreviewProviderAgentImport(
		    const std::string& provider_id, const std::filesystem::path& source_path,
		    const std::string& source_entry = {}, const std::string& local_opencode_version = {});
		/// List the provider's native agent files (global and workspace folders) and OpenCode JSON(C) agent entries.
		static ProviderAgentDiscovery DiscoverProviderAgents(const std::string& provider_id,
		    const std::filesystem::path& data_root, const std::filesystem::path& workspace_root);
		static bool ImportProviderAgent(const std::filesystem::path& data_root,
		                                const std::filesystem::path& workspace_root,
		                                const ProviderAgentImportRequest& request,
		                                AgentDefinition* imported_out = nullptr,
		                                std::string* error_out = nullptr);
		/// <summary>Write or replace a global agent under data_root/agents after canonical validation.</summary>
		static bool SaveGlobalAgent(const std::filesystem::path& data_root, const AgentDefinition& agent, std::string* error_out = nullptr);
		static bool DeleteGlobalAgent(const std::filesystem::path& data_root, const std::string& id, std::string* error_out = nullptr);
		static std::string ExecutionCapabilityForProvider(const std::string& provider_id);
		/// <summary>Read a validated provider-bound policy from the immutable canonical snapshot.</summary>
		/// Decode an immutable primary snapshot without reading the current definition.
		static bool DecodeCapturedPrimary(const std::string& agent_id, const std::string& snapshot,
		    AgentDefinition& definition, std::string* error_out = nullptr);
		static bool CapturedOpenCodePermission(const std::string& agent_id, const std::string& snapshot,
		                                      std::string& permission_json, std::string* error_out = nullptr);
		static bool ValidateCapturedRuntimePolicy(const std::string& agent_id, const std::string& snapshot,
		                                         const std::string& provider_id, const std::string& execution_capability,
		                                         std::string* error_out = nullptr);
		static bool PrepareRuntimeAdapter(const std::filesystem::path& data_root,
		                                  const std::string& chat_id,
		                                  const std::string& provider_id,
		                                  const std::string& agent_id,
		                                  const std::string& instructions,
		                                  ProviderAgentRuntimeAdapter* adapter_out,
		                                  std::string* error_out = nullptr,
		                                  const std::string* definition_snapshot = nullptr);
	};
} // namespace uam
