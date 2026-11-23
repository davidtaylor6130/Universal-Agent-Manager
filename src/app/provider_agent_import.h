#pragma once

#include "app/agent_definition_service.h"

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Provider-agent import: a closed YAML frontmatter subset, OpenCode JSON(C) agent entries,
/// restriction normalisation and native agent discovery. Nothing here evaluates arbitrary YAML.
namespace uam::provider_agent_import
{
	struct Node
	{
		enum class Kind
		{
			Null,
			Scalar,
			Sequence,
			Map,
			Opaque
		};
		Kind kind = Kind::Null;
		std::string text;
		bool quoted = false;
		std::vector<Node> items;
		std::vector<std::pair<std::string, Node>> entries;

		// Out of line: a vector of pairs holding the incomplete Node is not portable with an implicit definition.
		Node();
		Node(const Node&);
		Node(Node&&) noexcept;
		Node& operator=(const Node&);
		Node& operator=(Node&&) noexcept;
		~Node();

		const Node* Find(std::string_view key) const;
	};

	/// Config-level `tools` or `permission` that OpenCode applies before an agent's own rules.
	struct InheritedRules
	{
		std::string label; // origin prefix in errors and reasons
		std::string field; // "tools" or "permission"
		Node node;
		std::string files; // the config files that contributed, for the preview note
	};

	struct ParsedSource
	{
		std::vector<std::pair<std::string, Node>> fields;
		std::vector<InheritedRules> inherited;
		std::string instructions;
		/// A JSON agent's `{file:...}` prompt, relative to its config, shown in the preview.
		std::string prompt_file;
		std::string error;

		const Node* Find(std::string_view key) const;
	};

	inline constexpr std::size_t kMaxNestingDepth = 4;
	inline constexpr std::size_t kMaxCollectionEntries = 256;
	inline constexpr std::size_t kMaxImportedDescriptionBytes = 1024;

	/// Parse the frontmatter lines between `---` markers. Fields outside `interpreted` keep only their
	/// key (their block is skipped unparsed). Errors name the offending field path.
	bool ParseFrontmatter(const std::vector<std::string>& lines, const std::set<std::string>& interpreted,
	                      std::vector<std::pair<std::string, Node>>& fields, std::string& error);

	/// Split a Markdown agent into frontmatter fields and its instruction body.
	bool ParseMarkdownAgent(const std::string& text, const std::set<std::string>& interpreted, ParsedSource& out);

	/// Remove `//` and `/* */` comments and trailing commas outside strings.
	std::string StripJsonc(std::string_view text);

	/// Read one `agent`/`agents` entry of an OpenCode JSON(C) config. `{file:...}` prompts must stay inside the config's folder.
	bool ParseJsonConfigAgent(const std::string& text, const std::string& entry,
	                            const std::filesystem::path& config_path, ParsedSource& out);

	/// Add root `tools`/`permission` from every OpenCode config that applies to `source_path` (global, then the project's
	/// root and `.opencode` configs), merged so a deny in any of them is kept. A config that exists but cannot be read is an error.
	bool AddInheritedConfigRules(const std::filesystem::path& source_path, ParsedSource& out);

	/// Agent entry names and descriptions in an OpenCode JSON(C) config, in file order.
	std::vector<std::pair<std::string, std::string>> ListJsonConfigAgents(const std::string& text, std::string& error);

	/// Lowercase a provider name into a UAM agent ID (`a-z`, `0-9`, `-`), or empty when nothing usable remains.
	std::string SuggestId(std::string_view raw);

	/// Collapse whitespace, swap double quotes and bound to `limit` bytes on a UTF-8 boundary.
	std::string NormalizeDescription(std::string_view raw, std::size_t limit, std::vector<std::string>* notes);

	/// Normalise a parsed source into a preview. `file_id` is the Markdown file stem or JSON entry.
	ProviderAgentImportPreview BuildPreview(const std::string& provider_id, ParsedSource source,
	                                        const std::string& file_id, const std::string& local_opencode_version);

	/// Apply the chosen access ceiling to normalised rules. Returns the canonical policy to bind, or empty when
	/// workspace access alone expresses every rule.
	std::string BoundPolicyForAccess(const ProviderAgentImportPreview& preview, const std::string& workspace_access);

	/// True when stored OpenCode rules are a valid canonical policy for the given workspace access.
	bool ValidStoredPolicy(const std::string& policy_json, const std::string& workspace_access);

	/// Tighten a stored policy for read access (write tools denied); returns empty on invalid input.
	std::string TightenPolicyForRead(const std::string& policy_json);

	/// Same rules with the same last-match reading; key order alone does not make a re-import a change.
	bool PoliciesEquivalent(const std::string& left, const std::string& right);

	ProviderAgentDiscovery Discover(const std::string& provider_id, const std::filesystem::path& data_root,
	                                const std::filesystem::path& workspace_root);
} // namespace uam::provider_agent_import
