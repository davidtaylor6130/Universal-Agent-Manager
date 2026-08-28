#include "app/agent_definition_service.h"

#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::uintmax_t kMaxDefinitionBytes = 256U * 1024U;
		constexpr std::size_t kMaxInstructionsBytes = 192U * 1024U;
		constexpr std::size_t kMaxFrontmatterBytes = 16U * 1024U;
		constexpr std::size_t kMaxDescriptionBytes = 240;
		constexpr std::size_t kMaxListItems = 16;
		constexpr std::size_t kMaxIdBytes = 64;
		constexpr std::size_t kMaxFilesPerScope = 256;
		constexpr std::uintmax_t kMaxDefinitionBytesPerScope = 8U * 1024U * 1024U;
		constexpr std::size_t kMaxVisibleErrors = 64;
		constexpr std::size_t kMaxErrorBytes = 512;

		std::string BoundedError(std::string_view value)
		{
			std::string bounded;
			bounded.reserve(std::min(value.size(), kMaxErrorBytes));
			for (const unsigned char ch : value)
			{
				bounded.push_back(ch < 0x20 || ch == 0x7f ? ' ' : static_cast<char>(ch));
				if (bounded.size() == kMaxErrorBytes) break;
			}
			return uam::strings::Trim(bounded);
		}

		class ErrorCollector
		{
		  public:
			explicit ErrorCollector(std::vector<std::string>* errors) : errors_(errors) {}

			void Add(std::string value)
			{
				if (errors_->size() < kMaxVisibleErrors - 1)
				{
					errors_->push_back(BoundedError(value));
				}
				else
				{
					++omitted_;
				}
			}

			void Finish()
			{
				if (omitted_ > 0)
				{
					errors_->push_back(std::to_string(omitted_) + " additional agent definition diagnostics omitted.");
				}
			}

		  private:
			std::vector<std::string>* errors_;
			std::size_t omitted_ = 0;
		};

		bool ValidId(std::string_view value)
		{
			if (value.empty() || value.size() > kMaxIdBytes || value.front() == '-' || value.back() == '-') return false;
			return std::ranges::all_of(value, [](unsigned char ch) {
				return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
			});
		}

		bool HasDisallowedControl(std::string_view value)
		{
			return std::ranges::any_of(value, [](unsigned char ch) {
				return ch == 0 || (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t');
			});
		}

		std::string Unquote(std::string_view value)
		{
			value = uam::strings::TrimAsciiView(value);
			if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
			                          (value.front() == '\'' && value.back() == '\'')))
			{
				return std::string(value.substr(1, value.size() - 2));
			}
			return std::string(value);
		}

		std::optional<std::vector<std::string>> ParseIdList(std::string_view raw)
		{
			raw = uam::strings::TrimAsciiView(raw);
			if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') return std::nullopt;
			raw.remove_prefix(1);
			raw.remove_suffix(1);
			std::vector<std::string> values;
			std::unordered_set<std::string> seen;
			std::size_t start = 0;
			while (start <= raw.size())
			{
				const std::size_t comma = raw.find(',', start);
				const std::size_t end = comma == std::string_view::npos ? raw.size() : comma;
				const std::string value = uam::strings::TrimAndLowerAscii(Unquote(raw.substr(start, end - start)));
				if (!value.empty())
				{
					if (!ValidId(value) || values.size() >= kMaxListItems || !seen.insert(value).second) return std::nullopt;
					values.push_back(value);
				}
				if (comma == std::string_view::npos) break;
				start = comma + 1;
			}
			return values;
		}

		bool PathComponentsAreSafe(const std::filesystem::path& scope_root,
		                           const std::filesystem::path& agent_root)
		{
			const std::filesystem::path scope = uam::paths::AbsolutePathNoThrow(scope_root);
			const std::filesystem::path root = uam::paths::AbsolutePathNoThrow(agent_root);
			if (!uam::paths::IsSameOrInsideRoot(scope, root)) return false;

			const std::filesystem::path relative = root.lexically_relative(scope);
			if (relative.empty() || relative.is_absolute()) return false;
			std::filesystem::path current = scope;
			for (const std::filesystem::path& component : relative)
			{
				if (component == "." || component == "..") return false;
				current /= component;
				if (uam::paths::IsLinkOrReparsePointNoThrow(current)) return false;
			}
			return true;
		}

		std::optional<std::uintmax_t> ValidDefinitionFileSize(const std::filesystem::path& root,
		                                                         const std::filesystem::path& path)
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			const std::optional<std::filesystem::path> relative = uam::paths::RelativePathIfInsideRoot(root, path);
			if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
			    uam::paths::IsLinkOrReparsePointNoThrow(path) || !relative.has_value() || !relative->parent_path().empty())
			{
				return std::nullopt;
			}
			return uam::paths::FileSizeNoThrow(path);
		}

		std::optional<AgentDefinition> ParseFile(const std::filesystem::path& root,
		                                         const std::filesystem::path& path,
		                                         bool workspace_override,
		                                         std::uintmax_t* aggregate_bytes_read,
		                                         std::string* error_out)
		{
			const std::optional<std::uintmax_t> size = ValidDefinitionFileSize(root, path);
			if (!size.has_value())
			{
				*error_out = "Agent definition must be a direct regular file, not a link or traversal.";
				return std::nullopt;
			}
			if (*size == 0 || *size > kMaxDefinitionBytes)
			{
				*error_out = "Agent definition must be between 1 byte and 256 KiB.";
				return std::nullopt;
			}
			if (*aggregate_bytes_read > kMaxDefinitionBytesPerScope ||
			    *size > kMaxDefinitionBytesPerScope - *aggregate_bytes_read)
			{
				*error_out = "Agent definition was not read because the agent directory exceeds the 8 MiB scan limit.";
				return std::nullopt;
			}
			std::string text;
			if (!uam::io::TryReadTextFile(path, text))
			{
				*error_out = "Agent definition could not be read.";
				return std::nullopt;
			}
			const std::optional<std::uintmax_t> size_after_read = ValidDefinitionFileSize(root, path);
			if (!size_after_read.has_value() || *size_after_read != *size || text.size() != *size || HasDisallowedControl(text))
			{
				*error_out = "Agent definition changed type, path, or size while reading, or contains control characters.";
				return std::nullopt;
			}
			*aggregate_bytes_read += *size;

			std::istringstream lines(text);
			std::string line;
			if (!std::getline(lines, line) || uam::strings::Trim(line) != "---")
			{
				*error_out = "Agent definition requires frontmatter.";
				return std::nullopt;
			}
			std::map<std::string, std::string> fields;
			bool closed = false;
			std::size_t frontmatter_bytes = 4;
			while (std::getline(lines, line))
			{
				frontmatter_bytes += line.size() + 1;
				if (frontmatter_bytes > kMaxFrontmatterBytes)
				{
					*error_out = "Agent frontmatter exceeds 16 KiB.";
					return std::nullopt;
				}
				if (uam::strings::Trim(line) == "---")
				{
					closed = true;
					break;
				}
				const std::size_t colon = line.find(':');
				if (colon == std::string::npos)
				{
					*error_out = "Agent frontmatter contains a malformed field.";
					return std::nullopt;
				}
				const std::string key = uam::strings::Trim(line.substr(0, colon));
				if (!std::set<std::string>{"version", "name", "description", "mode", "workspaceAccess", "skills", "delegates"}.contains(key) ||
				    !fields.emplace(key, uam::strings::Trim(line.substr(colon + 1))).second)
				{
					*error_out = "Agent frontmatter contains an unknown or duplicate field: " + key;
					return std::nullopt;
				}
			}
			std::ostringstream body;
			body << lines.rdbuf();
			AgentDefinition result;
			result.id = uam::strings::TrimAndLowerAscii(Unquote(fields.contains("name") ? fields["name"] : uam::paths::Utf8PathString(path.stem())));
			result.description = Unquote(fields["description"]);
			result.mode = uam::strings::TrimAndLowerAscii(Unquote(fields["mode"]));
			result.workspace_access = uam::strings::TrimAndLowerAscii(Unquote(fields["workspaceAccess"]));
			result.instructions = uam::strings::Trim(body.str());
			const auto skills = ParseIdList(fields.contains("skills") ? fields["skills"] : "[]");
			const auto delegates = ParseIdList(fields.contains("delegates") ? fields["delegates"] : "[]");
			if (!closed || Unquote(fields["version"]) != "1" || !ValidId(result.id) ||
			    result.id != uam::strings::TrimAndLowerAscii(uam::paths::Utf8PathString(path.stem())) ||
			    result.id == "build" || result.id == "plan" || result.description.empty() || result.description.size() > kMaxDescriptionBytes ||
			    (result.mode != "primary" && result.mode != "subagent" && result.mode != "both") ||
			    (result.workspace_access != "read" && result.workspace_access != "write") ||
			    !skills.has_value() || !delegates.has_value() || result.instructions.empty() ||
			    result.instructions.size() > kMaxInstructionsBytes)
			{
				*error_out = "Agent definition has an invalid version, name, mode, access, list, description, or instruction body.";
				return std::nullopt;
			}
			result.skills = *skills;
			result.delegates = *delegates;
			result.definition_hash = "fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(text));
			result.markdown_snapshot = std::move(text);
			result.source_path = path;
			result.workspace_override = workspace_override;
			return result;
		}

		void LoadDirectory(const std::filesystem::path& scope_root,
		                   const std::filesystem::path& root, bool workspace_override,
		                   std::map<std::string, AgentDefinition>* definitions,
		                   ErrorCollector* errors)
		{
			if (!PathComponentsAreSafe(scope_root, root) || uam::paths::IsLinkOrReparsePointNoThrow(root))
			{
				errors->Add(uam::paths::Utf8PathString(root) + ": agent root is linked, outside its scope, or not a real directory.");
				return;
			}
			if (!uam::paths::PathExistsNoThrow(root)) return;
			if (!uam::paths::IsDirectoryNoThrow(root))
			{
				errors->Add(uam::paths::Utf8PathString(root) + ": agent root is not a real directory.");
				return;
			}
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
			{
				if (uam::strings::TrimAndLowerAscii(uam::paths::Utf8PathString(it->path().extension())) != ".md") continue;
				if (files.size() == kMaxFilesPerScope)
				{
					errors->Add(uam::paths::Utf8PathString(root) + ": agent directory exceeds the 256-file limit; the scope was not loaded.");
					return;
				}
				files.push_back(it->path());
			}
			if (error)
			{
				errors->Add(uam::paths::Utf8PathString(root) + ": agent directory could not be listed.");
				return;
			}
			std::ranges::sort(files, {}, [](const auto& path) { return uam::paths::PortablePathString(path.filename()); });
			std::unordered_set<std::string> scope_ids;
			std::uintmax_t aggregate_bytes_read = 0;
			for (const auto& file : files)
			{
				std::string parse_error;
				const std::optional<AgentDefinition> parsed = ParseFile(root, file, workspace_override, &aggregate_bytes_read, &parse_error);
				if (!parsed.has_value())
				{
					const std::string claimed_id = uam::strings::TrimAndLowerAscii(uam::paths::Utf8PathString(file.stem()));
					if (workspace_override && ValidId(claimed_id) && claimed_id != "build" && claimed_id != "plan") definitions->erase(claimed_id);
					errors->Add(uam::paths::Utf8PathString(file) + ": " + parse_error);
					continue;
				}
				if (!scope_ids.insert(parsed->id).second)
				{
					definitions->erase(parsed->id);
					errors->Add(uam::paths::Utf8PathString(file) + ": duplicate agent identity " + parsed->id + ".");
					continue;
				}
				(*definitions)[parsed->id] = *parsed;
			}
		}

		bool HasCycle(const std::string& id, const std::map<std::string, AgentDefinition>& definitions,
		              std::unordered_set<std::string>* visiting, std::unordered_set<std::string>* visited)
		{
			if (visited->contains(id)) return false;
			if (!visiting->insert(id).second) return true;
			for (const std::string& delegate : definitions.at(id).delegates)
			{
				if (definitions.contains(delegate) && HasCycle(delegate, definitions, visiting, visited)) return true;
			}
			visiting->erase(id);
			visited->insert(id);
			return false;
		}

		std::string NativeProviderId(std::string_view value)
		{
			const std::string provider = provider_ids::NormalizeCliProviderAliasOrSelf(value);
			return provider == provider_ids::kOpenCodeCli || provider == provider_ids::kCopilotCli ||
			               provider == provider_ids::kGeminiCli || provider == provider_ids::kClaudeCli
			           ? provider
			           : std::string{};
		}

		std::string NativeAgentFileId(const std::filesystem::path& path)
		{
			std::string filename = uam::strings::TrimAndLowerAscii(uam::paths::Utf8PathString(path.filename()));
			if (filename.ends_with(".agent.md")) filename.resize(filename.size() - 9);
			else if (filename.ends_with(".md")) filename.resize(filename.size() - 3);
			return filename;
		}

		std::string IdListMarkdown(const std::vector<std::string>& values)
		{
			std::string text = "[";
			for (std::size_t index = 0; index < values.size(); ++index)
			{
				if (index > 0) text += ", ";
				text += values[index];
			}
			return text + "]";
		}
	} // namespace

	AgentDefinitionCatalog AgentDefinitionService::Load(const std::filesystem::path& data_root,
	                                                    const std::filesystem::path& workspace_root)
	{
		std::map<std::string, AgentDefinition> definitions;
		definitions.emplace("build", AgentDefinition{.id = "build", .description = "Implement and verify changes within the chat permission policy.", .mode = "primary", .workspace_access = "write", .instructions = "Implement the requested change and verify it.", .definition_hash = "builtin-build-v1", .markdown_snapshot = "builtin:build:v1", .built_in = true});
		definitions.emplace("plan", AgentDefinition{.id = "plan", .description = "Inspect and plan without modifying the workspace.", .mode = "primary", .workspace_access = "read", .instructions = "Inspect the task and produce a concrete read-only plan.", .definition_hash = "builtin-plan-v1", .markdown_snapshot = "builtin:plan:v1", .built_in = true});

		AgentDefinitionCatalog catalog;
		ErrorCollector errors(&catalog.errors);
		LoadDirectory(data_root, data_root / "agents", false, &definitions, &errors);
		if (!workspace_root.empty()) LoadDirectory(workspace_root, workspace_root / ".uam" / "agents", true, &definitions, &errors);

		std::unordered_set<std::string> invalid;
		for (const auto& [id, definition] : definitions)
		{
			for (const std::string& delegate : definition.delegates)
			{
				if (!definitions.contains(delegate) || delegate == id)
				{
					invalid.insert(id);
					errors.Add("Agent " + id + " has an unavailable or self delegate: " + delegate + ".");
				}
			}
		}
		for (bool changed = true; changed;)
		{
			changed = false;
			for (const auto& [id, definition] : definitions)
			{
				if (invalid.contains(id)) continue;
				if (std::ranges::any_of(definition.delegates, [&](const std::string& delegate) { return invalid.contains(delegate); }))
				{
					invalid.insert(id);
					errors.Add("Agent " + id + " depends on an invalid delegate.");
					changed = true;
				}
			}
		}
		std::unordered_set<std::string> visiting;
		std::unordered_set<std::string> visited;
		for (const auto& [id, definition] : definitions)
		{
			(void)definition;
			visiting.clear();
			if (HasCycle(id, definitions, &visiting, &visited))
			{
				invalid.insert(visiting.begin(), visiting.end());
				errors.Add("Agent delegation cycle includes " + id + ".");
			}
		}
		for (bool changed = true; changed;)
		{
			changed = false;
			for (const auto& [id, definition] : definitions)
			{
				if (invalid.contains(id)) continue;
				if (std::ranges::any_of(definition.delegates, [&](const std::string& delegate) { return invalid.contains(delegate); }))
				{
					invalid.insert(id);
					errors.Add("Agent " + id + " depends on an invalid delegate.");
					changed = true;
				}
			}
		}
		for (const std::string& id : invalid) definitions.erase(id);
		for (auto& [id, definition] : definitions)
		{
			(void)id;
			catalog.definitions.push_back(std::move(definition));
		}
		errors.Finish();
		return catalog;
	}

	ProviderAgentImportPreview AgentDefinitionService::PreviewProviderAgentImport(
	    const std::string& provider_id, const std::filesystem::path& source_path)
	{
		ProviderAgentImportPreview preview;
		preview.provider_id = NativeProviderId(provider_id);
		preview.source_path = uam::paths::AbsolutePathNoThrow(source_path);
		if (preview.provider_id.empty())
		{
			preview.error = "Only OpenCode, Copilot CLI, Gemini CLI, and Claude Code Markdown agents are supported for import.";
			return preview;
		}
		std::error_code error;
		const auto status = std::filesystem::symlink_status(preview.source_path, error);
		const auto size = uam::paths::FileSizeNoThrow(preview.source_path);
		if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
		    uam::paths::IsLinkOrReparsePointNoThrow(preview.source_path) || !size.has_value() ||
		    *size == 0 || *size > kMaxDefinitionBytes)
		{
			preview.error = "Provider agent source must be a regular, non-linked Markdown file no larger than 256 KiB.";
			return preview;
		}
		std::string text;
		if (!uam::io::TryReadTextFile(preview.source_path, text) || text.size() != *size || HasDisallowedControl(text))
		{
			preview.error = "Provider agent source could not be read safely.";
			return preview;
		}

		std::istringstream lines(text);
		std::string line;
		if (!std::getline(lines, line) || uam::strings::Trim(line) != "---")
		{
			preview.error = "Provider agent source requires YAML frontmatter.";
			return preview;
		}
		std::map<std::string, std::string> fields;
		bool closed = false;
		std::size_t frontmatter_bytes = 4;
		while (std::getline(lines, line))
		{
			frontmatter_bytes += line.size() + 1;
			if (frontmatter_bytes > kMaxFrontmatterBytes)
			{
				preview.error = "Provider agent frontmatter exceeds 16 KiB.";
				return preview;
			}
			if (uam::strings::Trim(line) == "---")
			{
				closed = true;
				break;
			}
			if (line.empty() || line.front() == ' ' || line.front() == '\t') continue;
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos)
			{
				preview.error = "Provider agent frontmatter contains a malformed top-level field.";
				return preview;
			}
			const std::string key = uam::strings::Trim(line.substr(0, colon));
			if (key.empty() || !fields.emplace(key, uam::strings::Trim(line.substr(colon + 1))).second)
			{
				preview.error = "Provider agent frontmatter contains an empty or duplicate field.";
				return preview;
			}
		}
		std::ostringstream body;
		body << lines.rdbuf();
		preview.instructions = uam::strings::Trim(body.str());
		if (!closed || preview.instructions.empty() || preview.instructions.size() > kMaxInstructionsBytes)
		{
			preview.error = "Provider agent frontmatter is unclosed or its instruction body is empty or oversized.";
			return preview;
		}

		preview.suggested_id = preview.provider_id == provider_ids::kGeminiCli && fields.contains("name")
		                           ? uam::strings::TrimAndLowerAscii(Unquote(fields["name"]))
		                           : NativeAgentFileId(preview.source_path);
		preview.description = fields.contains("description") ? Unquote(fields["description"]) : std::string{};
		if (preview.provider_id == provider_ids::kOpenCodeCli)
		{
			preview.mode = uam::strings::TrimAndLowerAscii(Unquote(fields["mode"]));
			if (preview.mode == "all") preview.mode = "both";
		}
		else preview.mode = "subagent";

		const std::set<std::string> security_fields{
		    "tools", "allowed-tools", "permission", "mcp-servers", "mcpServers", "max_turns",
		    "maxTurns", "steps", "timeout_mins", "disallowed-tools", "disallowedTools",
		    "permission-mode", "permissionMode", "hooks", "memory", "background", "isolation", "skills"};
		const std::set<std::string> mapped_fields = preview.provider_id == provider_ids::kOpenCodeCli
		                                                       ? std::set<std::string>{"description", "mode"}
		                                                       : preview.provider_id == provider_ids::kCopilotCli ||
		                                                                 preview.provider_id == provider_ids::kClaudeCli
		                                                             ? std::set<std::string>{"name", "description"}
		                                                             : std::set<std::string>{"name", "description", "kind"};
		for (const auto& [key, value] : fields)
		{
			(void)value;
			if (security_fields.contains(key)) preview.security_fields.push_back(key);
			else if (!mapped_fields.contains(key)) preview.ignored_fields.push_back(key);
		}
		if (preview.provider_id == provider_ids::kGeminiCli && fields.contains("kind") &&
		    uam::strings::TrimAndLowerAscii(Unquote(fields["kind"])) != "local")
		{
			preview.security_fields.push_back("kind");
		}
		std::ranges::sort(preview.security_fields);
		preview.security_fields.erase(std::unique(preview.security_fields.begin(), preview.security_fields.end()), preview.security_fields.end());
		std::ranges::sort(preview.ignored_fields);
		if (!preview.security_fields.empty())
		{
			preview.error = "Import is blocked because provider security/tool fields cannot be mapped without changing their meaning.";
			return preview;
		}
		if (!ValidId(preview.suggested_id) || preview.suggested_id == "build" || preview.suggested_id == "plan" ||
		    preview.description.empty() || preview.description.size() > kMaxDescriptionBytes ||
		    (preview.mode != "primary" && preview.mode != "subagent" && preview.mode != "both"))
		{
			preview.error = "Provider agent has an invalid name, description, or mode for the UAM schema.";
			return preview;
		}
		preview.supported = true;
		return preview;
	}

	bool AgentDefinitionService::ImportProviderAgent(const std::filesystem::path& data_root,
	                                                const std::filesystem::path& workspace_root,
	                                                const ProviderAgentImportRequest& request,
	                                                AgentDefinition* imported_out,
	                                                std::string* error_out)
	{
		if (imported_out != nullptr) *imported_out = AgentDefinition{};
		const ProviderAgentImportPreview preview = PreviewProviderAgentImport(request.provider_id, request.source_path);
		const std::string id = uam::strings::TrimAndLowerAscii(
		    request.canonical_id.empty() ? preview.suggested_id : request.canonical_id);
		if (!preview.supported)
		{
			if (error_out != nullptr) *error_out = preview.error;
			return false;
		}
		if (!preview.ignored_fields.empty() && !request.acknowledge_ignored_fields)
		{
			if (error_out != nullptr) *error_out = "Provider-only fields would be omitted; preview and explicitly acknowledge them before import.";
			return false;
		}
		if (!ValidId(id) || id == "build" || id == "plan" ||
		    (request.workspace_access != "read" && request.workspace_access != "write") ||
		    request.skills.size() > kMaxListItems || !request.delegates.empty() ||
		    !std::ranges::all_of(request.skills, [](const std::string& value) { return ValidId(value); }))
		{
			if (error_out != nullptr) *error_out = "Import requires a valid canonical ID, explicit read/write access, valid skills, and no pre-authorised delegates.";
			return false;
		}

		const std::filesystem::path scope = request.workspace_scope ? workspace_root : data_root;
		if (scope.empty())
		{
			if (error_out != nullptr) *error_out = "The selected agent scope is unavailable.";
			return false;
		}
		const std::filesystem::path root = request.workspace_scope ? scope / ".uam" / "agents" : scope / "agents";
		std::error_code create_error;
		std::filesystem::create_directories(root, create_error);
		if (create_error || !PathComponentsAreSafe(scope, root) || uam::paths::IsLinkOrReparsePointNoThrow(root))
		{
			if (error_out != nullptr) *error_out = "The managed agent directory could not be created safely.";
			return false;
		}
		const std::filesystem::path target = root / (id + ".md");
		if (uam::paths::IsSameOrInsideRoot(root, preview.source_path) ||
		    (uam::paths::PathExistsNoThrow(target) && !request.replace_existing))
		{
			if (error_out != nullptr) *error_out = "Import source is already managed or the target agent already exists.";
			return false;
		}
		const std::string markdown = "---\nversion: 1\nname: " + id + "\ndescription: \"" +
		                             preview.description + "\"\nmode: " + preview.mode +
		                             "\nworkspaceAccess: " + request.workspace_access + "\nskills: " +
		                             IdListMarkdown(request.skills) + "\ndelegates: []\n---\n" +
		                             preview.instructions + "\n";
		if (!uam::io::WriteTextFile(target, markdown))
		{
			if (error_out != nullptr) *error_out = "The imported UAM agent could not be written atomically.";
			return false;
		}
		std::uintmax_t bytes = 0;
		std::string parse_error;
		const auto imported = ParseFile(root, target, request.workspace_scope, &bytes, &parse_error);
		if (!imported.has_value())
		{
			if (error_out != nullptr) *error_out = "The imported UAM agent failed canonical validation: " + parse_error;
			return false;
		}
		if (imported_out != nullptr) *imported_out = *imported;
		if (error_out != nullptr) error_out->clear();
		return true;
	}

	std::string AgentDefinitionService::ExecutionCapabilityForProvider(const std::string& provider_id)
	{
		const std::string provider = provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
		if (provider == provider_ids::kOpenCodeCli) return "opencode-native-agent-config";
		if (provider == provider_ids::kCopilotCli) return "copilot-native-agent-plugin";
		return "uam-prompt-injected";
	}

	bool AgentDefinitionService::PrepareRuntimeAdapter(const std::filesystem::path& data_root,
	                                                   const std::string& chat_id,
	                                                   const std::string& provider_id,
	                                                   const std::string& agent_id,
	                                                   const std::string& instructions,
	                                                   ProviderAgentRuntimeAdapter* adapter_out,
	                                                   std::string* error_out)
	{
		if (adapter_out == nullptr || data_root.empty() || chat_id.empty() || !ValidId(agent_id) ||
		    instructions.empty() || instructions.size() > kMaxInstructionsBytes || HasDisallowedControl(instructions))
		{
			if (error_out != nullptr) *error_out = "The UAM agent runtime snapshot is invalid.";
			return false;
		}
		ProviderAgentRuntimeAdapter adapter;
		adapter.execution_capability = ExecutionCapabilityForProvider(provider_id);
		if (adapter.execution_capability == "uam-prompt-injected")
		{
			*adapter_out = std::move(adapter);
			return true;
		}

		const std::string provider = provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
		const std::string key = uam::hashing::Hex64Padded(
		    uam::hashing::Fnv1a64(chat_id + "\n" + provider));
		const std::string native_id = "uam-" + key;
		adapter.directory = data_root / "runtime" / "agent-adapters" / key;
		if (uam::paths::IsLinkOrReparsePointNoThrow(adapter.directory) ||
		    !uam::paths::CreateDirectoriesNoThrow(adapter.directory))
		{
			if (error_out != nullptr) *error_out = "Could not create isolated UAM agent runtime storage.";
			return false;
		}

		if (provider == provider_ids::kOpenCodeCli)
		{
			const std::filesystem::path agents = adapter.directory / "agent";
			const nlohmann::json permission{{"*", "ask"},
			                                {"task", "deny"},
			                                {"uam-computer_computer_observe", "allow"},
			                                {"uam-computer_computer_action", "allow"}};
			if (uam::paths::IsLinkOrReparsePointNoThrow(agents) || !uam::paths::CreateDirectoriesNoThrow(agents) ||
			    !uam::io::WriteTextFile(adapter.directory / "opencode.json",
			                            nlohmann::json{{"$schema", "https://opencode.ai/config.json"},
			                                           {"default_agent", native_id}, {"subagent_depth", 0},
			                                           {"experimental", {{"mcp_timeout", 120000}}},
			                                           {"permission", permission}}.dump(2) + "\n") ||
			    !uam::io::WriteTextFile(agents / (native_id + ".md"),
			                            "---\ndescription: UAM-managed agent\nmode: primary\npermission:\n  \"*\": ask\n  task: deny\n  uam-computer_computer_observe: allow\n  uam-computer_computer_action: allow\n---\n" + instructions + "\n"))
			{
				if (error_out != nullptr) *error_out = "Could not write the isolated OpenCode agent configuration.";
				return false;
			}
			adapter.launch_environment.emplace_back("OPENCODE_CONFIG_DIR", uam::paths::Utf8PathString(adapter.directory));
			adapter.inject_in_prompt = false;
		}
		else
		{
			const std::filesystem::path agents = adapter.directory / "agents";
			if (uam::paths::IsLinkOrReparsePointNoThrow(agents) || !uam::paths::CreateDirectoriesNoThrow(agents) ||
			    !uam::io::WriteTextFile(adapter.directory / "plugin.json",
			                            nlohmann::json{{"name", "uam-agent-" + key},
			                                           {"description", "UAM-managed runtime agent"},
			                                           {"version", "1.0.0"}, {"agents", "agents/"}}.dump(2) + "\n") ||
			    !uam::io::WriteTextFile(agents / (native_id + ".agent.md"),
			                            "---\nname: " + native_id + "\ndescription: UAM-managed agent\ninfer: false\n---\n" + instructions + "\n"))
			{
				if (error_out != nullptr) *error_out = "Could not write the isolated Copilot agent plugin.";
				return false;
			}
			adapter.launch_arguments = {"--plugin-dir", uam::paths::Utf8PathString(adapter.directory),
			                            "--agent", native_id,
			                            "--excluded-tools=task,read_agent,list_agents,write_agent"};
			adapter.inject_in_prompt = false;
		}
		*adapter_out = std::move(adapter);
		if (error_out != nullptr) error_out->clear();
		return true;
	}
} // namespace uam
