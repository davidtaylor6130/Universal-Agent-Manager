#pragma once

#include "app/agent_definition_service.h"
#include "app/markdown_store_service.h"
#include "common/provider/provider_setup.h"
#include "common/models/app_models.h"

namespace uam
{
	struct ProviderConfigurationBundle
	{
		nlohmann::json manifest;
		std::map<std::string, std::string> files;
		std::filesystem::path local_directory;
	};

	/// <summary>Compile native launch resources; preserve the queued agent instructions when supplied. SSH receives bytes and unresolved secret references.</summary>
	inline bool PrepareProviderConfiguration(const std::filesystem::path& data_root, const AppSettings& settings,
	    const ChatSession& chat, const ExecutionHost& host, const std::filesystem::path& workspace,
	    ProviderConfigurationBundle& bundle, std::string& error, const std::string* selected_agent_instructions = nullptr)
	{
		// Source workspace settings follow isolated worktrees; the active workspace can override them.
		std::vector<McpServerConfiguration> candidates = settings.mcp_servers;
		if (!chat.workspace_source_directory.empty() && chat.workspace_source_directory != uam::paths::Utf8PathString(workspace))
		{
			std::stable_partition(candidates.begin(), candidates.end(), [&](const McpServerConfiguration& server)
			{
				return !server.workspace_directory.empty() && mcp_server_config::WorkspaceKey(server.workspace_directory) == mcp_server_config::WorkspaceKey(chat.workspace_source_directory);
			});
			for (McpServerConfiguration& server : candidates)
				if (!server.workspace_directory.empty() && mcp_server_config::WorkspaceKey(server.workspace_directory) == mcp_server_config::WorkspaceKey(chat.workspace_source_directory))
					server.workspace_directory = uam::paths::Utf8PathString(workspace);
		}
		const std::vector<McpServerConfiguration> selected_servers = mcp_server_config::SelectForWorkspace(
		    candidates, uam::paths::Utf8PathString(workspace), host.id);
		if (!settings.central_provider_configuration.enabled && selected_servers.empty()) return true;
		const CentralProviderConfiguration& configuration = settings.central_provider_configuration;
		std::set<std::string> executable_files;
		if (configuration.enabled)
		{
			std::size_t resource_bytes = configuration.instructions.size();
			std::string instructions = configuration.instructions;
			for (const std::string& source : configuration.instruction_files)
			{
				std::string bytes;
				if (!provider_setup::ReadFile(uam::paths::PathFromUtf8(source), bytes, error)) return false;
				resource_bytes += bytes.size();
				if (resource_bytes > provider_setup::kMaxFileBytes) { error = "Central instructions exceed 2 MiB."; return false; }
				instructions += "\n\n" + bytes;
			}
			const AgentDefinitionCatalog catalog = AgentDefinitionService::Load(data_root, host.transport == "ssh" ? std::filesystem::path{} : workspace);
			const std::vector<AgentDefinition>::const_iterator selected = std::find_if(catalog.definitions.begin(), catalog.definitions.end(), [&](const AgentDefinition& agent) { return agent.id == chat.uam_agent_id; });
			if (selected == catalog.definitions.end()) { error = "The selected UAM agent is unavailable: " + chat.uam_agent_id; return false; }
			instructions += "\n\n" + (selected_agent_instructions ? *selected_agent_instructions : selected->instructions);
			instructions += "\n\nUAM agent definitions are available below. Delegate through UAM tools only when permitted by the selected agent.\n";
			for (const AgentDefinition& agent : catalog.definitions)
			{
				const std::string name = "agents/" + agent.id + ".md";
				if (!provider_setup::SafeRelativePath(name)) { error = "An agent id cannot be installed safely."; return false; }
				bundle.files[name] = selected_agent_instructions && agent.id == selected->id ? *selected_agent_instructions
				    : agent.built_in || agent.markdown_snapshot.empty() ? agent.instructions : agent.markdown_snapshot;
				instructions += "- " + agent.id + ": __UAM_RESOURCE_ROOT__/" + name + "\n";
			}
			instructions += "\nAvailable skills: read the skill's SKILL.md before using it. Relative script and reference paths resolve inside that skill directory.\n";
			std::set<std::string> skill_names;
			for (const std::string& source : configuration.skill_directories)
			{
				const std::filesystem::path root = uam::paths::PathFromUtf8(source);
				std::error_code ec;
				if (!provider_setup::NoSymlinks(root) || !std::filesystem::is_directory(root, ec) || ec)
				{ error = "Central skill folder is missing or a symbolic link: " + source; return false; }
				std::vector<std::filesystem::path> skills;
				if (std::filesystem::is_regular_file(root / "SKILL.md", ec)) skills.push_back(root);
				else
				{
					for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, ec))
						if (entry.is_directory(ec) && std::filesystem::is_regular_file(entry.path() / "SKILL.md", ec)) skills.push_back(entry.path());
					if (ec) { error = "Could not list central skill folders."; return false; }
				}
				for (const std::filesystem::path& skill : skills)
				{
					const std::string name = uam::paths::Utf8PathString(skill.filename());
					if (!skill_names.insert(name).second) { error = "Central skill folders contain the same skill name: " + name; return false; }
					instructions += "- " + name + ": __UAM_RESOURCE_ROOT__/skills/" + name + "/SKILL.md\n";
					for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(skill, ec))
					{
						if (entry.is_symlink(ec)) { error = "Central skill assets cannot contain symbolic links."; return false; }
						if (entry.is_directory(ec)) continue;
						const std::string destination = "skills/" + name + "/" + uam::paths::PortablePathString(entry.path().lexically_relative(skill));
						if (!provider_setup::SafeRelativePath(destination) || bundle.files.size() >= provider_setup::kMaxFiles)
						{ error = "Central skill assets contain an invalid path or too many files."; return false; }
						std::string bytes;
						if (!provider_setup::ReadFile(entry.path(), bytes, error)) return false;
						resource_bytes += bytes.size();
						if (resource_bytes > provider_setup::kMaxBundleBytes) { error = "Central resources exceed 32 MiB."; return false; }
						bundle.files[destination] = std::move(bytes);
						if ((entry.status(ec).permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) != std::filesystem::perms::none) executable_files.insert(destination);
					}
					if (ec) { error = "Could not read central skill assets."; return false; }
				}
			}
			if (!settings.markdown_store_directory.empty())
			{
				const std::vector<MarkdownStoreService::Entry> entries = MarkdownStoreService::ListEntries(MarkdownStoreService::NormalizeRoot(settings.markdown_store_directory), &error);
				if (!error.empty()) return false;
				for (const MarkdownStoreService::Entry& entry : entries)
				{
					const std::string name = entry.command_name;
					if (!provider_setup::SafeRelativePath(name) || name.find('/') != std::string::npos) { error = "A library skill name cannot be installed safely."; return false; }
					if (!skill_names.insert(name).second) continue;
					instructions += "- " + name + ": __UAM_RESOURCE_ROOT__/skills/" + name + "/SKILL.md\n";
					resource_bytes += entry.body.size();
					if (resource_bytes > provider_setup::kMaxBundleBytes) { error = "Central resources exceed 32 MiB."; return false; }
					bundle.files["skills/" + name + "/SKILL.md"] = entry.body;
					const std::filesystem::path source = uam::paths::PathFromUtf8(entry.source_path);
					std::error_code ec;
					if (source.filename() != "SKILL.md" || !std::filesystem::is_regular_file(source, ec)) continue;
					for (const std::filesystem::directory_entry& asset : std::filesystem::recursive_directory_iterator(source.parent_path(), ec))
					{
						if (asset.is_symlink(ec)) { error = "Imported skill assets cannot contain symbolic links."; return false; }
						if (asset.is_directory(ec) || asset.path() == source) continue;
						const std::string destination = "skills/" + name + "/" + uam::paths::PortablePathString(asset.path().lexically_relative(source.parent_path()));
						if (!provider_setup::SafeRelativePath(destination) || bundle.files.size() >= provider_setup::kMaxFiles) { error = "An imported skill contains invalid paths or too many assets."; return false; }
						std::string bytes;
						if (!provider_setup::ReadFile(asset.path(), bytes, error)) return false;
						resource_bytes += bytes.size();
						if (resource_bytes > provider_setup::kMaxBundleBytes) { error = "Central resources exceed 32 MiB."; return false; }
						bundle.files[destination] = std::move(bytes);
						if ((asset.status(ec).permissions() & (std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec)) != std::filesystem::perms::none) executable_files.insert(destination);
					}
					if (ec) { error = "Could not read imported skill assets."; return false; }
				}
			}
			bundle.files["AGENTS.md"] = std::move(instructions);
		}
		std::vector<McpServerConfiguration> servers;
		for (McpServerConfiguration server : selected_servers)
		{
			if (!server.enabled || (!server.execution_host_id.empty() && server.execution_host_id != host.id)) continue;
			if (!server.workspace_directory.empty() && mcp_server_config::WorkspaceKey(server.workspace_directory) != mcp_server_config::WorkspaceKey(uam::paths::Utf8PathString(workspace))) continue;
			server.execution_host_id.clear();
			server.workspace_directory.clear();
			servers.push_back(std::move(server));
		}
		bundle.manifest = {{"format", 1}, {"mcpOnly", !configuration.enabled}, {"files", nlohmann::json::object()},
		    {"hostInstructionFile", configuration.enabled ? host.instruction_file : ""}, {"mcpServers", mcp_server_config::Serialize(servers)}};
		for (const auto& [name, bytes] : bundle.files)
			bundle.manifest["files"][name] = {{"size", bytes.size()}, {"digest", provider_setup::Digest(bytes)}, {"executable", executable_files.contains(name)}};
		bundle.local_directory = data_root / "runtime" / "provider-setup" / provider_setup::Digest(bundle.manifest.dump());
		std::vector<std::string> missing;
		if (!provider_setup::Prepare(bundle.local_directory, bundle.manifest, missing, error)) return false;
		for (const std::string& name : missing)
		{
			const std::filesystem::path target = bundle.local_directory / uam::paths::PathFromUtf8(name);
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			if (ec || !provider_setup::WriteRuntimeFile(target, bundle.files.at(name), error, false)) { error = "Could not install central resource: " + name; return false; }
		}
		return true;
	}
}
