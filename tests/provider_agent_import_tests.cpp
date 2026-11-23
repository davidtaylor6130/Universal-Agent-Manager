#include "test_harness.h"

#include "app/provider_agent_import.h"

#include <tuple>

using namespace uam_test;

namespace
{
	namespace import = uam::provider_agent_import;

	fs::path WriteSource(const fs::path& path, const std::string& text)
	{
		fs::create_directories(path.parent_path());
		UAM_ASSERT(uam::io::WriteTextFile(path, text));
		return path;
	}

	bool Contains(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}

	bool AnyNoteContains(const uam::ProviderAgentImportPreview& preview, const std::string& needle)
	{
		return std::ranges::any_of(preview.notes, [&](const std::string& note) { return Contains(note, needle); });
	}

	uam::ProviderAgentImportRequest Request(const std::string& provider, const fs::path& source, const std::string& access,
	                                        const std::string& entry = {})
	{
		uam::ProviderAgentImportRequest request;
		request.provider_id = provider;
		request.source_path = source;
		request.source_entry = entry;
		request.workspace_access = access;
		request.acknowledge_ignored_fields = true;
		return request;
	}

	std::string Frontmatter(const std::string& fields, const std::string& body = "Do the work and report.")
	{
		return "---\n" + fields + "---\n" + body + "\n";
	}

	std::vector<std::string> Lines(const std::string& text)
	{
		std::vector<std::string> lines;
		std::size_t start = 0;
		while (start <= text.size())
		{
			const std::size_t end = text.find('\n', start);
			lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
			if (end == std::string::npos) break;
			start = end + 1;
		}
		if (!lines.empty() && lines.back().empty()) lines.pop_back();
		return lines;
	}
} // namespace

UAM_TEST(ProviderAgentFrontmatterParsesTheClosedYamlSubset)
{
	const std::set<std::string> interpreted{"description", "tools", "permission", "list", "flow", "quoted", "keep", "strip"};
	std::vector<std::pair<std::string, import::Node>> fields;
	std::string error;
	const std::string text =
	    "# leading comment\n"
	    "description: >-\n"
	    "  Reviews code\n"
	    "  carefully.\n"
	    "\n"
	    "  Second paragraph.\n"
	    "tools:\n"
	    "  write: false # comment\n"
	    "  'bash': true\n"
	    "permission:\n"
	    "  edit: deny\n"
	    "  bash:\n"
	    "    \"*\": ask\n"
	    "    \"git status\": allow\n"
	    "list:\n"
	    "- Read\n"
	    "- \"Grep\"\n"
	    "flow: {edit: deny, \"*\": ask}\n"
	    "quoted: \"first line\n"
	    "  second \\\"line\\\"\"\n"
	    "keep: |+\n"
	    "  a\n"
	    "\n"
	    "strip: |-\n"
	    "  b\n"
	    "hooks:\n"
	    "  PreToolUse:\n"
	    "    - matcher: Bash\n"
	    "      hooks: [{type: command}]\n";
	UAM_ASSERT(import::ParseFrontmatter(Lines(text), interpreted, fields, error));
	const auto find = [&](const std::string& key) { return std::ranges::find_if(fields, [&](const auto& field) { return field.first == key; })->second; };
	UAM_ASSERT_EQ(find("description").text, std::string("Reviews code carefully.\nSecond paragraph."));
	UAM_ASSERT_EQ(find("tools").entries.size(), std::size_t{2});
	UAM_ASSERT_EQ(find("tools").Find("write")->text, std::string("false"));
	UAM_ASSERT_EQ(find("tools").Find("bash")->text, std::string("true"));
	UAM_ASSERT_EQ(find("permission").Find("bash")->Find("git status")->text, std::string("allow"));
	UAM_ASSERT_EQ(find("list").items.size(), std::size_t{2});
	UAM_ASSERT_EQ(find("list").items[1].text, std::string("Grep"));
	UAM_ASSERT_EQ(find("flow").Find("*")->text, std::string("ask"));
	UAM_ASSERT_EQ(find("quoted").text, std::string("first line second \"line\""));
	UAM_ASSERT_EQ(find("keep").text, std::string("a\n\n"));
	UAM_ASSERT_EQ(find("strip").text, std::string("b"));
	UAM_ASSERT(find("hooks").kind == import::Node::Kind::Opaque);
}

UAM_TEST(ProviderAgentFrontmatterRejectsUnsafeYamlNamingTheField)
{
	const std::set<std::string> interpreted{"description", "tools", "permission"};
	const std::vector<std::pair<std::string, std::string>> cases{
	    {"permission: &policy\n  edit: deny\n", "permission:"},
	    {"permission: *policy\n", "permission:"},
	    {"permission:\n  <<: *policy\n", "permission:"},
	    {"permission:\n  edit: !!str deny\n", "permission.edit:"},
	    {"permission:\n\tedit: deny\n", "permission:"},
	    {"permission:\n  edit: deny\n  edit: allow\n", "permission.edit: duplicate key"},
	    {"permission:\n  edit: deny\n    bash: ask\n", "permission.edit: a continuation line looks like a key"},
	    {"description: Reviews\n  permission:\n    edit: allow\n", "description: a continuation line looks like a key"},
	    {"tools: [read, [edit]]\n", "tools:"},
	    {"tools: [read,\n  edit]\n", "tools:"},
	    {"permission: {bash: {\"*\": ask}}\n", "permission:"},
	    {"description: \"never closed\n", "description:"},
	    {"description: \"bad \\x escape\"\n", "description:"},
	    {"\"\\u0070ermission\": deny\n", "frontmatter line 2:"},
	    {"permission:\n  ? complex\n", "permission:"},
	    {"tools:\n  - name: read\n", "tools[0]:"},
	    {"permission:\n  a:\n    b:\n      c:\n        d: deny\n", "permission.a.b.c:"},
	    {"description: one\ndescription: two\n", "description: duplicate field"},
	    {"  description: indented\n", "frontmatter line 2:"},
	};
	for (const auto& [text, expected] : cases)
	{
		std::vector<std::pair<std::string, import::Node>> fields;
		std::string error;
		UAM_ASSERT(!import::ParseFrontmatter(Lines(text), interpreted, fields, error));
		if (!error.starts_with(expected)) throw TestFailure("Expected error starting '" + expected + "', got '" + error + "' for: " + text);
	}
}

UAM_TEST(OpenCodeImportAcceptsEveryReportedMacAgentShapeWithinItsCeiling)
{
	TempDir temp("uam-opencode-reported-shapes");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path data = temp.root / "data";
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	std::string error;

	// Shape 1: a tools map plus permission {edit: allow, bash: ask}.
	const fs::path builder = WriteSource(temp.root / "native" / "builder.md", Frontmatter(
	    "description: Builds features\nmode: subagent\nmodel: anthropic/claude\ntools:\n  write: true\n  edit: true\n  bash: true\npermission:\n  edit: allow\n  bash: ask\n"));
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, builder);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(!preview.provider_bound);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("write"));
	UAM_ASSERT_EQ(preview.suggested_workspace_access, std::string("write"));
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"bash":"ask","edit":"ask","write":"ask"})"));
	UAM_ASSERT(AnyNoteContains(preview, "allow becomes ask"));
	uam::AgentDefinition imported;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, builder, "write"), &imported, &error));
	UAM_ASSERT_EQ(imported.workspace_access, std::string("write"));
	UAM_ASSERT(imported.open_code_permission_json.empty());

	// Shape 2: a tools map only, disabling edit tools and a web tool.
	const fs::path reader = WriteSource(temp.root / "native" / "reader.md", Frontmatter(
	    "description: Reads only\ntools:\n  write: false\n  edit: false\n  bash: true\n  webfetch: false\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, reader);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(preview.provider_bound);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT(Contains(preview.read_only_reason, "tools.edit"));
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, reader, "write"), nullptr, &error));
	UAM_ASSERT(error.starts_with("workspaceAccess:"));
	UAM_ASSERT(!fs::exists(data / "agents" / "reader.md"));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, reader, "read"), &imported, &error));
	UAM_ASSERT_EQ(imported.open_code_permission_json, std::string(R"({"bash":"ask","edit":"deny","webfetch":"deny","write":"deny"})"));

	// A disabled write tool alone denies every edit tool, but read access already expresses it.
	const fs::path no_write = WriteSource(temp.root / "native" / "no-write.md", Frontmatter("description: No writes\ntools:\n  write: false\n  bash: true\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, no_write);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(!preview.provider_bound);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT(Contains(preview.open_code_permission_json, R"("edit":"deny")"));

	// Shape 3: a permission map denying every tool, kept rather than refused.
	for (const std::string& policy : {std::string("permission:\n  \"*\": deny\n"), std::string("permission: deny\n"),
	                                  std::string("permission:\n  edit: deny\n  bash: deny\n  read: deny\n  webfetch: deny\n")})
	{
		const fs::path locked = WriteSource(temp.root / "native" / "locked.md", Frontmatter("description: Locked down\n" + policy));
		preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, locked);
		UAM_ASSERT(preview.supported);
		UAM_ASSERT(preview.provider_bound);
		UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
		auto request = Request(opencode, locked, "read");
		request.replace_existing = true;
		fs::remove(data / "agents" / "locked.md");
		UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, request, &imported, &error));
		UAM_ASSERT(Contains(imported.open_code_permission_json, R"("edit":"deny")"));
		UAM_ASSERT(!Contains(imported.open_code_permission_json, R"("ask")"));
	}

	// Shape 4: edit: allow with a 330-byte description.
	const std::string long_description(330, 'd');
	const fs::path verbose = WriteSource(temp.root / "native" / "verbose.md", Frontmatter("description: " + long_description + "\npermission:\n  edit: allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, verbose);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.description, long_description);
	UAM_ASSERT_EQ(preview.suggested_workspace_access, std::string("write"));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, verbose, "write"), &imported, &error));
	UAM_ASSERT_EQ(imported.description, long_description);

	// The previously passing shape still imports, now without tying the agent to OpenCode.
	const fs::path reviewer = WriteSource(temp.root / "native" / "reviewer.md", Frontmatter("description: Reviews\npermission:\n  edit: deny\n  bash: ask\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, reviewer);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(!preview.provider_bound);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, reviewer, "read"), &imported, &error));
	UAM_ASSERT(imported.open_code_permission_json.empty());

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(data, {});
	UAM_ASSERT(catalog.errors.empty());
	for (const std::string id : {"builder", "reader", "locked", "verbose", "reviewer"})
		UAM_ASSERT(std::ranges::find(catalog.definitions, id, &uam::AgentDefinition::id) != catalog.definitions.end());
}

UAM_TEST(OpenCodeImportKeepsDeniesAndNamesTheOffendingField)
{
	TempDir temp("uam-opencode-rule-normalisation");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path source = temp.root / "agent.md";

	WriteSource(source, Frontmatter("description: Shell\npermission:\n  bash:\n    \"*\": ask\n    \"git status\": allow\n    \"rm *\": deny\n"));
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(preview.provider_bound);
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"bash":"deny"})"));
	UAM_ASSERT(AnyNoteContains(preview, "permission.bash: pattern rules"));
	UAM_ASSERT(AnyNoteContains(preview, "local OpenCode 1.x"));

	WriteSource(source, Frontmatter("description: Shell\npermission:\n  bash:\n    \"*\": allow\n    \"git *\": ask\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"bash":"ask"})"));

	const std::vector<std::pair<std::string, std::string>> failures{
	    {"permission:\n  edit: maybe\n", "permission.edit: expected allow, ask or deny"},
	    {"permission:\n  bash:\n    \"rm *\": sometimes\n", "permission.bash.rm *: expected allow, ask or deny"},
	    {"tools:\n  bash: yes please\n", "tools.bash: expected true or false"},
	    {"tools: [read, write]\n", "tools: expected a map"},
	    {"permission:\n  \"ed it\": deny\n", "permission.ed it: unsupported tool name"},
	    {"mode: everything\n", "mode: expected primary, subagent or all"},
	    {"mode:\n", "mode: expected primary, subagent or all"},
	    {"hooks:\n  Stop: []\n", "hooks:"},
	};
	for (const auto& [fields, expected] : failures)
	{
		WriteSource(source, Frontmatter("description: Rules\n" + fields));
		preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
		UAM_ASSERT(!preview.supported);
		if (!preview.error.starts_with(expected)) throw TestFailure("Expected '" + expected + "', got '" + preview.error + "'");
	}
	WriteSource(source, "---\ndescription: Missing end\n");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source).error.starts_with("frontmatter:"));
	WriteSource(source, "---\ndescription: No body\n---\n");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source).error.starts_with("instructions:"));
}

UAM_TEST(ProviderAgentImportShortensLongAndBlockDescriptionsVisibly)
{
	TempDir temp("uam-agent-import-descriptions");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path source = temp.root / "agent.md";
	std::string block = "description: |\n";
	for (int line = 0; line < 40; ++line) block += "  Use this agent when the user says \"review\" and wants a detailed report line " + std::to_string(line) + ".\n";
	WriteSource(source, Frontmatter("name: Code Reviewer\n" + block));
	const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.suggested_id, std::string("code-reviewer"));
	UAM_ASSERT(preview.description.size() <= import::kMaxImportedDescriptionBytes);
	UAM_ASSERT(preview.description.ends_with("\xE2\x80\xA6"));
	UAM_ASSERT(preview.description.find('\n') == std::string::npos);
	UAM_ASSERT(preview.description.find('"') == std::string::npos);
	UAM_ASSERT(AnyNoteContains(preview, "description: shortened from"));
	UAM_ASSERT(AnyNoteContains(preview, "double quotes became single quotes"));
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, Request(uam::provider_ids::kClaudeCli, source, "read"), &imported, &error));
	UAM_ASSERT_EQ(imported.description, preview.description);

	// UTF-8 is never split, and a missing description gets a placeholder rather than an error.
	std::string utf8 = "description: ";
	for (int index = 0; index < 600; ++index) utf8 += "\xC3\xA9";
	WriteSource(source, Frontmatter(utf8 + "\n"));
	const auto accented = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, source);
	UAM_ASSERT(accented.supported);
	UAM_ASSERT_EQ((accented.description.size() - 3) % 2, std::size_t{0});
	WriteSource(source, Frontmatter("mode: primary\n"));
	const auto missing = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, source);
	UAM_ASSERT(missing.supported);
	UAM_ASSERT(AnyNoteContains(missing, "description: none in the source"));
}

UAM_TEST(ClaudeGeminiAndCopilotAgentsImportWithToolListsMappedToTheirCeiling)
{
	TempDir temp("uam-agent-import-native-tools");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path data = temp.root / "data";
	std::string error;
	uam::AgentDefinition imported;

	const fs::path claude = WriteSource(temp.root / "claude" / "code-reviewer.md", Frontmatter(
	    "name: code-reviewer\ndescription: Use this agent to review code.\n  It reports issues.\ntools: Read, Grep, Glob\nmodel: sonnet\ncolor: blue\npermissionMode: default\nskills:\n  - review\n"));
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.description, std::string("Use this agent to review code. It reports issues."));
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(preview.read_only_reason, std::string("tools lists only read tools"));
	UAM_ASSERT_EQ(preview.ignored_fields, (std::vector<std::string>{"color", "model", "permissionMode", "skills", "tools"}));
	UAM_ASSERT(AnyNoteContains(preview, "tools: UAM keeps the workspace-access limit only"));
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(uam::provider_ids::kClaudeCli, claude, "write"), nullptr, &error));
	UAM_ASSERT(error.starts_with("workspaceAccess: tools lists only read tools"));
	auto unacknowledged = Request(uam::provider_ids::kClaudeCli, claude, "read");
	unacknowledged.acknowledge_ignored_fields = false;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(data, {}, unacknowledged, nullptr, &error));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(uam::provider_ids::kClaudeCli, claude, "read"), &imported, &error));
	UAM_ASSERT_EQ(imported.id, std::string("code-reviewer"));

	const fs::path claude_writer = WriteSource(temp.root / "claude" / "fixer.md", Frontmatter("name: fixer\ndescription: Fixes\ntools:\n  - Read\n  - Edit\n  - Bash(git:*)\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude_writer);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("write"));
	UAM_ASSERT_EQ(preview.suggested_workspace_access, std::string("write"));

	const fs::path claude_denied = WriteSource(temp.root / "claude" / "denied.md", Frontmatter("name: denied\ndescription: No edits\ndisallowedTools: Write, Edit, MultiEdit, NotebookEdit, Bash\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude_denied);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	const fs::path claude_plan = WriteSource(temp.root / "claude" / "planner.md", Frontmatter("name: planner\ndescription: Plans\npermissionMode: plan\n"));
	UAM_ASSERT_EQ(uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude_plan).max_workspace_access, std::string("read"));

	const fs::path claude_hooks = WriteSource(temp.root / "claude" / "hooked.md", Frontmatter(
	    "name: hooked\ndescription: Guarded\nhooks:\n  PreToolUse:\n    - matcher: \"Bash\"\n      hooks:\n        - type: command\n          command: ./guard.sh\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude_hooks);
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT_EQ(preview.security_fields, std::vector<std::string>{"hooks"});
	UAM_ASSERT(preview.error.starts_with("hooks:"));

	const fs::path gemini = WriteSource(temp.root / "gemini" / "investigator.md", Frontmatter(
	    "name: investigator\ndescription: Investigates code\nkind: local\ntools:\n  - read_file\n  - search_file_content\nmodel: gemini-2.5-pro\ntemperature: 0.2\nmax_turns: 10\ntimeout_mins: 5\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kGeminiCli, gemini);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(preview.ignored_fields, (std::vector<std::string>{"max_turns", "model", "temperature", "timeout_mins", "tools"}));
	UAM_ASSERT(AnyNoteContains(preview, "max_turns: UAM does not copy turn"));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(uam::provider_ids::kGeminiCli, gemini, "read"), &imported, &error));
	const fs::path gemini_remote = WriteSource(temp.root / "gemini" / "remote.md", Frontmatter("name: remote\ndescription: Remote\nkind: remote\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kGeminiCli, gemini_remote);
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT(preview.error.starts_with("kind:"));

	const fs::path copilot = WriteSource(temp.root / "copilot" / "docs-writer.agent.md", Frontmatter(
	    "name: Docs Writer\ndescription: 'Writes documentation'\ntools: ['read', 'edit', 'search']\nmodel: gpt-5\ninfer: false\nmcp-servers:\n  github:\n    type: local\n    command: npx\n    args: [\"-y\", \"server\"]\n    env:\n      TOKEN: ${{ secrets.TOKEN }}\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kCopilotCli, copilot);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.suggested_id, std::string("docs-writer"));
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("write"));
	UAM_ASSERT_EQ(preview.suggested_workspace_access, std::string("write"));
	UAM_ASSERT(AnyNoteContains(preview, "mcp-servers: not copied"));
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(uam::provider_ids::kCopilotCli, copilot, "write"), &imported, &error));
	UAM_ASSERT_EQ(imported.workspace_access, std::string("write"));
	UAM_ASSERT(imported.open_code_permission_json.empty());
}

UAM_TEST(OpenCodeJsoncAgentEntriesPreviewAndImportWithFilePrompts)
{
	TempDir temp("uam-opencode-json-agents");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path config = WriteSource(temp.root / "project" / "opencode.jsonc",
	    "{\n"
	    "  // Project agents\n"
	    "  \"$schema\": \"https://opencode.ai/config.json\",\n"
	    "  \"agent\": {\n"
	    "    \"reviewer\": {\n"
	    "      \"description\": \"Reviews // not a comment\",\n"
	    "      \"mode\": \"subagent\",\n"
	    "      \"prompt\": \"{file:./prompts/reviewer.md}\",\n"
	    "      \"tools\": { \"write\": false, \"edit\": false, },\n"
	    "      \"permission\": { \"bash\": { \"*\": \"ask\", \"rm *\": \"deny\" } },\n"
	    "      /* model is provider-only */\n"
	    "      \"model\": \"anthropic/claude\",\n"
	    "    },\n"
	    "    \"build\": { \"model\": \"x/y\" },\n"
	    "  },\n"
	    "  \"agents\": { \"writer\": { \"description\": \"Writes\", \"prompt\": \"Write it.\", \"permission\": { \"edit\": \"allow\" } } },\n"
	    "}\n");
	WriteSource(temp.root / "project" / "prompts" / "reviewer.md", "Review carefully.\n");
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "reviewer");
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.source_entry, std::string("reviewer"));
	UAM_ASSERT_EQ(preview.instructions, std::string("Review carefully."));
	UAM_ASSERT_EQ(preview.description, std::string("Reviews // not a comment"));
	UAM_ASSERT_EQ(preview.mode, std::string("subagent"));
	UAM_ASSERT(preview.provider_bound);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(preview.ignored_fields, std::vector<std::string>{"model"});
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, Request(opencode, config, "read", "reviewer"), &imported, &error));
	UAM_ASSERT_EQ(imported.open_code_permission_json, std::string(R"({"bash":"deny","edit":"deny","write":"deny"})"));

	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "writer");
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.suggested_workspace_access, std::string("write"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "build");
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT(preview.error.starts_with("instructions:"));
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "missing").error.starts_with("agent.missing:"));
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, config, "reviewer").error.starts_with("source:"));

	WriteSource(config, "{ \"agent\": { \"a\": { \"description\": \"x\", \"prompt\": \"{file:../../outside.md}\" } } }");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "a").error.starts_with("prompt:"));
	WriteSource(config, "{ \"agent\": { \"a\": { \"description\": \"x\", \"prompt\": \"Use {env:SECRET}\" } } }");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "a").error.starts_with("prompt:"));
	WriteSource(config, "{ \"agent\": { \"a\": { \"description\": \"x\", \"prompt\": \"p\", \"permission\": { \"edit\": \"deny\", \"edit\": \"allow\" } } } }");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "a").error.starts_with("config: duplicate key edit"));
}

UAM_TEST(OpenCodeTwoRefusesProviderBoundRulesAtPreviewOnly)
{
	TempDir temp("uam-opencode-v2-import-preview");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path bound = WriteSource(temp.root / "bound.md", Frontmatter("description: Bound\npermission:\n  bash: deny\n"));
	const fs::path neutral = WriteSource(temp.root / "neutral.md", Frontmatter("description: Neutral\npermission:\n  edit: deny\n  bash: ask\n"));
	const auto refused = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, bound, {}, "2.2.7");
	UAM_ASSERT(!refused.supported);
	UAM_ASSERT(refused.error.starts_with("permission: OpenCode 2.2.7"));
	UAM_ASSERT(refused.error.find('\n') == std::string::npos);
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, bound, {}, "1.19.4").supported);
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, bound, {}, "").supported);
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, neutral, {}, "2.2.7").supported);
	auto request = Request(opencode, bound, "read");
	request.local_opencode_version = "2.2.7";
	std::string error;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(error.starts_with("permission: OpenCode 2.2.7"));
}

UAM_TEST(ImportedOpenCodeRulesStayBoundThroughEditsAndRuntimeCapture)
{
	TempDir temp("uam-opencode-bound-edit");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path data = temp.root / "data";
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path source = WriteSource(temp.root / "native" / "web.md", Frontmatter("description: No web\npermission:\n  edit: ask\n  webfetch: deny\n"));
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, source, "write"), &imported, &error));
	UAM_ASSERT_EQ(imported.open_code_permission_json, std::string(R"({"edit":"ask","webfetch":"deny"})"));
	std::string permission;
	UAM_ASSERT(uam::AgentDefinitionService::CapturedOpenCodePermission("web", imported.markdown_snapshot, permission, &error));
	UAM_ASSERT_EQ(permission, imported.open_code_permission_json);
	UAM_ASSERT(uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("web", imported.markdown_snapshot, opencode, "opencode-native-agent-config", &error));
	UAM_ASSERT(!uam::AgentDefinitionService::ValidateCapturedRuntimePolicy("web", imported.markdown_snapshot, opencode, "uam-central-native-setup", &error));

	// Narrowing to read access tightens the rules; widening back cannot reopen the denied edits.
	uam::AgentDefinition edited = imported;
	edited.open_code_permission_json.clear();
	edited.workspace_access = "read";
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(data, edited, &error));
	UAM_ASSERT(uam::AgentDefinitionService::CapturedOpenCodePermission("web", uam::io::ReadTextFile(data / "agents" / "web.md"), permission, &error));
	UAM_ASSERT_EQ(permission, std::string(R"({"webfetch":"deny","edit":"deny"})"));
	const std::string tightened = uam::io::ReadTextFile(data / "agents" / "web.md");
	edited.workspace_access = "write";
	UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(data, edited, &error));
	UAM_ASSERT(error.starts_with("workspaceAccess:"));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(data / "agents" / "web.md"), tightened);

	// Stored rules never carry allow and must match their workspace access.
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"edit":"allow"})", "write"));
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"edit":"ask"})", "read"));
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"edit":"deny"})", "write"));
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"*":"deny"})", "write"));
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"bash":{"*":"deny"}})", "read"));
	UAM_ASSERT(import::ValidStoredPolicy(R"({"*":"deny"})", "read"));
	UAM_ASSERT(import::ValidStoredPolicy(R"({"edit":"deny","bash":"ask"})", "read"));
}

UAM_TEST(ProviderAgentDiscoveryListsGlobalWorkspaceAndJsonAgents)
{
	TempDir temp("uam-provider-agent-discovery");
	const fs::path home = temp.root / "home";
	const fs::path workspace = temp.root / "workspace";
	const fs::path data = temp.root / "data";
	ScopedEnvVar home_env("HOME", home.string());
	ScopedEnvVar profile_env("USERPROFILE", home.string());
	ScopedEnvVar xdg_env("XDG_CONFIG_HOME", (home / ".config").string());
	WriteSource(home / ".claude" / "agents" / "reviewer.md", Frontmatter("name: code-reviewer\ndescription: |\n  Reviews code\n  in detail.\n"));
	WriteSource(workspace / ".claude" / "agents" / "local.md", Frontmatter("name: local\ndescription: Local\n"));
	WriteSource(home / ".claude" / "agents" / "notes.txt", "ignored");
	WriteSource(home / ".gemini" / "agents" / "helper.md", Frontmatter("name: helper\ndescription: Helps\n"));
	WriteSource(home / ".copilot" / "agents" / "docs.agent.md", Frontmatter("name: Docs\ndescription: Docs\n"));
	WriteSource(workspace / ".github" / "agents" / "ci.agent.md", Frontmatter("description: CI\n"));
	WriteSource(home / ".config" / "opencode" / "agent" / "global.md", Frontmatter("description: Global\n"));
	WriteSource(workspace / ".opencode" / "agents" / "project.md", Frontmatter("description: Project\n"));
	WriteSource(workspace / "opencode.json", R"({"agent": {"json-agent": {"description": "From JSON", "prompt": "Go."}}})");
	WriteSource(home / ".config" / "opencode" / "opencode.jsonc", "{ // comment\n \"agents\": { \"tidy\": { \"description\": \"Tidies\", \"prompt\": \"Tidy.\" }, }, }");

	const auto claude = uam::AgentDefinitionService::DiscoverProviderAgents("claude", data, workspace);
	UAM_ASSERT(claude.unsupported.empty());
	UAM_ASSERT_EQ(claude.candidates.size(), std::size_t{2});
	UAM_ASSERT_EQ(claude.candidates[0].id, std::string("code-reviewer"));
	UAM_ASSERT_EQ(claude.candidates[0].description, std::string("Reviews code in detail."));
	UAM_ASSERT_EQ(claude.candidates[0].scope, std::string("global"));
	UAM_ASSERT_EQ(claude.candidates[1].scope, std::string("workspace"));

	const auto opencode = uam::AgentDefinitionService::DiscoverProviderAgents(uam::provider_ids::kOpenCodeCli, data, workspace);
	std::vector<std::string> ids;
	for (const auto& candidate : opencode.candidates) ids.push_back(candidate.id + "|" + candidate.scope + "|" + candidate.source_entry);
	std::ranges::sort(ids);
	UAM_ASSERT_EQ(ids, (std::vector<std::string>{"global|global|", "json-agent|workspace|json-agent", "project|workspace|", "tidy|global|tidy"}));
	const auto json = std::ranges::find(opencode.candidates, std::string("json-agent"), &uam::ProviderAgentCandidate::id);
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, json->source_path, json->source_entry).supported);

	UAM_ASSERT_EQ(uam::AgentDefinitionService::DiscoverProviderAgents("gemini", data, workspace).candidates.size(), std::size_t{1});
	const auto copilot = uam::AgentDefinitionService::DiscoverProviderAgents("copilot", data, {});
	UAM_ASSERT_EQ(copilot.candidates.size(), std::size_t{1});
	UAM_ASSERT_EQ(copilot.candidates[0].id, std::string("docs"));
	UAM_ASSERT_EQ(uam::AgentDefinitionService::DiscoverProviderAgents("copilot", data, workspace).candidates.size(), std::size_t{2});
	UAM_ASSERT(!uam::AgentDefinitionService::DiscoverProviderAgents("codex", data, workspace).unsupported.empty());
	UAM_ASSERT(!uam::AgentDefinitionService::DiscoverProviderAgents("antigravity", data, workspace).unsupported.empty());

	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request("claude", claude.candidates[0].source_path, "read"), nullptr, &error));
	UAM_ASSERT(uam::AgentDefinitionService::DiscoverProviderAgents("claude", data, workspace).candidates[0].already_imported);
}

UAM_TEST(ProviderAgentImportRefusesBareCarriageReturnsThatHideFields)
{
	TempDir temp("uam-agent-import-bare-cr");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path source = WriteSource(temp.root / "agent.md", "---\ndescription: d\nmodel: x\rpermission:\r  edit: deny\n---\nbody\n");
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kOpenCodeCli, source);
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT(preview.error.starts_with("frontmatter: a carriage return"));
	WriteSource(source, "---\r\ndescription: d\rtools: Read, Grep\r\n---\r\nbody\r\n");
	UAM_ASSERT(!uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, source).supported);
	WriteSource(source, "---\r\nname: reader\r\ndescription: d\r\ntools: Read, Grep\r\n---\r\nbody\r\n");
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
}

UAM_TEST(OpenCodeImportFollowsLastMatchingRuleOrder)
{
	TempDir temp("uam-opencode-rule-order");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path source = temp.root / "agent.md";
	std::string error;
	// A later `*: deny` overrides earlier allows.
	WriteSource(source, Frontmatter("description: d\npermission:\n  edit: allow\n  bash: allow\n  \"*\": deny\n"));
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"*":"deny","bash":"deny","edit":"deny"})"));
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, Request(opencode, source, "write"), nullptr, &error));
	UAM_ASSERT(error.starts_with("workspaceAccess: permission.* is deny"));
	// A later exception after `*: deny` stays an exception, and `*` is stored first so the exception still wins.
	WriteSource(source, Frontmatter("description: d\npermission:\n  \"*\": deny\n  read: allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"*":"deny","read":"ask","edit":"deny"})"));
	// Disabled tools win over enabled ones in order, and over permission.
	WriteSource(source, Frontmatter("description: d\ntools:\n  read: true\n  \"*\": false\npermission:\n  read: allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT(Contains(preview.open_code_permission_json, R"("read":"deny")"));
	// A wildcard that covers edit caps access, and a read import keeps edit denied after it.
	WriteSource(source, Frontmatter("description: d\npermission:\n  \"ed*\": deny\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	WriteSource(source, Frontmatter("description: d\npermission:\n  bash: deny\n  \"edit*\": allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT(preview.supported);
	uam::AgentDefinition imported;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, Request(opencode, source, "read"), &imported, &error));
	UAM_ASSERT(import::ValidStoredPolicy(imported.open_code_permission_json, "read"));
	UAM_ASSERT(Contains(imported.open_code_permission_json, R"("edit*":"ask","bash":"deny","edit":"deny")"));
	// Wildcards that disagree both deny rather than depend on order.
	WriteSource(source, Frontmatter("description: d\npermission:\n  \"ba*\": allow\n  \"b*\": deny\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, source);
	UAM_ASSERT_EQ(preview.open_code_permission_json, std::string(R"({"b*":"deny","ba*":"deny"})"));
	// Stored rules are read in order too.
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"edit":"deny","edit*":"ask"})", "read"));
	UAM_ASSERT(!import::ValidStoredPolicy(R"({"ed*":"deny"})", "write"));
	UAM_ASSERT(import::ValidStoredPolicy(R"({"*":"deny","edit":"ask"})", "write"));
}

UAM_TEST(OpenCodeImportAppliesConfigLevelRules)
{
	TempDir temp("uam-opencode-config-rules");
	const fs::path xdg = temp.root / "xdg";
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", xdg.string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	// Root rules in the same JSON(C) config apply to its agents.
	const fs::path config = WriteSource(temp.root / "project" / "opencode.json", R"({"permission": {"edit": "deny"}, "agent": {"a": {"description": "x", "prompt": "Go."}}})");
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "a");
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT(Contains(preview.read_only_reason, "config permission.edit"));
	// A project Markdown agent inherits the project config; the agent's own later rule can still narrow it.
	WriteSource(temp.root / "project" / ".opencode" / "opencode.jsonc", "{ // project\n \"tools\": { \"webfetch\": false, }, }");
	const fs::path agent = WriteSource(temp.root / "project" / ".opencode" / "agents" / "web.md", Frontmatter("description: Web\npermission:\n  edit: allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, agent);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(preview.provider_bound);
	UAM_ASSERT(Contains(preview.open_code_permission_json, R"("webfetch":"deny")"));
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("write")); // the agent's later edit rule overrides the project deny
	UAM_ASSERT(AnyNoteContains(preview, "config: permission rules from opencode.json"));
	// The global config applies to every source; a broken one refuses rather than guessing.
	WriteSource(xdg / "opencode" / "opencode.json", R"({"permission": {"bash": "deny"}})");
	const fs::path plain = WriteSource(temp.root / "plain.md", Frontmatter("description: Plain\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain);
	UAM_ASSERT(preview.provider_bound);
	WriteSource(xdg / "opencode" / "opencode.json", "{broken");
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain);
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT(preview.error.starts_with("config: opencode.json"));
}

UAM_TEST(ReimportAfterNarrowingEditTreatsKeyOrderAsUnchanged)
{
	TempDir temp("uam-opencode-reimport-order");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path data = temp.root / "data";
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path source = WriteSource(temp.root / "web.md", Frontmatter("description: No web\npermission:\n  edit: ask\n  webfetch: deny\n"));
	uam::AgentDefinition imported;
	std::string error;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(opencode, source, "write"), &imported, &error));
	imported.workspace_access = "read";
	UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(data, imported, &error));
	auto request = Request(opencode, source, "read");
	request.replace_existing = true;
	UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, request, &imported, &error));
	UAM_ASSERT(import::PoliciesEquivalent(R"({"webfetch":"deny","edit":"deny"})", R"({"edit":"deny","webfetch":"deny"})"));
	UAM_ASSERT(!import::PoliciesEquivalent(R"({"edit":"deny","edit*":"ask"})", R"({"edit*":"ask","edit":"deny"})"));
}

UAM_TEST(ProviderAgentDiscoverySkipsLinkedParentFolders)
{
	TempDir temp("uam-provider-agent-discovery-links");
	const fs::path home = temp.root / "home";
	ScopedEnvVar home_env("HOME", home.string());
	ScopedEnvVar profile_env("USERPROFILE", home.string());
	WriteSource(temp.root / "elsewhere" / "agents" / "hidden.md", Frontmatter("name: hidden\ndescription: Hidden\n"));
	fs::create_directories(home);
	std::error_code link_error;
	fs::create_directory_symlink(temp.root / "elsewhere", home / ".claude", link_error);
	if (link_error) return; // Creating links needs privileges on some Windows runners.
	const auto discovery = uam::AgentDefinitionService::DiscoverProviderAgents("claude", temp.root / "data", {});
	UAM_ASSERT(discovery.candidates.empty());
	UAM_ASSERT(!discovery.errors.empty());
}

UAM_TEST(OpenCodeImportMergesEveryApplicableConfigStrictly)
{
	TempDir temp("uam-opencode-config-merge");
	const fs::path xdg = temp.root / "xdg";
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", xdg.string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	std::string error;
	// Two global files: OpenCode merges them key by key, so a later `*: ask` does not reopen an earlier edit deny.
	WriteSource(xdg / "opencode" / "config.json", R"({"permission": {"*": "ask", "edit": "deny"}})");
	WriteSource(xdg / "opencode" / "opencode.json", R"({"permission": {"*": "ask"}})");
	const fs::path plain = WriteSource(temp.root / "plain.md", Frontmatter("description: Plain\n"));
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, Request(opencode, plain, "write"), nullptr, &error));
	fs::remove(xdg / "opencode" / "config.json");
	fs::remove(xdg / "opencode" / "opencode.json");

	// An agent's own rules overlaid key by key cannot reopen what the config's later wildcard denies.
	WriteSource(xdg / "opencode" / "opencode.json", R"({"permission": {"edit": "ask", "*": "deny"}})");
	WriteSource(plain, Frontmatter("description: Plain\npermission:\n  edit: allow\n"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	fs::remove(xdg / "opencode" / "opencode.json");

	// A JSON entry in the project's root config also gets the project's .opencode config.
	const fs::path project = temp.root / "project";
	WriteSource(project / ".opencode" / "opencode.json", R"({"permission": {"edit": "deny"}})");
	const fs::path config = WriteSource(project / "opencode.json", R"({"agent": {"a": {"description": "x", "prompt": "Go."}}})");
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "a");
	UAM_ASSERT(preview.supported);
	UAM_ASSERT_EQ(preview.max_workspace_access, std::string("read"));
	// ...and an agent in the .opencode config gets the root jsonc beside the root json.
	fs::remove(project / ".opencode" / "opencode.json");
	WriteSource(project / "opencode.jsonc", R"({"tools": {"webfetch": false}})");
	const fs::path nested = WriteSource(project / ".opencode" / "opencode.json", R"({"agent": {"b": {"description": "y", "prompt": "Go."}}})");
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, nested, "b");
	UAM_ASSERT(preview.provider_bound);
	UAM_ASSERT(Contains(preview.open_code_permission_json, R"("webfetch":"deny")"));
}

UAM_TEST(OpenCodeImportFollowsLinkedAndEmptyGlobalConfigs)
{
	TempDir temp("uam-opencode-linked-config");
	const fs::path xdg = temp.root / "xdg";
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", xdg.string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	const fs::path plain = WriteSource(temp.root / "plain.md", Frontmatter("description: Plain\n"));
	WriteSource(xdg / "opencode" / "config.json", "");
	UAM_ASSERT(uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain).supported);
	const fs::path dotfiles = WriteSource(temp.root / "dotfiles" / "opencode.json", R"({"permission": {"bash": "deny"}})");
	std::error_code link_error;
	fs::create_symlink(dotfiles, xdg / "opencode" / "opencode.json", link_error);
	if (link_error) return; // Creating links needs privileges on some Windows runners.
	const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, plain);
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(preview.provider_bound);
}

UAM_TEST(ReadOnlyImportsKeepTheirCeilingThroughEdits)
{
	TempDir temp("uam-agent-import-ceiling");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const fs::path data = temp.root / "data";
	std::string error;
	const fs::path claude = WriteSource(temp.root / "reader.md", Frontmatter("name: reader\ndescription: Reads\ntools: Read, Grep\n"));
	const fs::path opencode = WriteSource(temp.root / "reviewer.md", Frontmatter("description: Reviews\npermission:\n  edit: deny\n  bash: ask\n"));
	for (const auto& [provider, source, id] : {std::tuple{std::string(uam::provider_ids::kClaudeCli), claude, std::string("reader")},
	                                           std::tuple{std::string(uam::provider_ids::kOpenCodeCli), opencode, std::string("reviewer")}})
	{
		uam::AgentDefinition imported;
		UAM_ASSERT(uam::AgentDefinitionService::ImportProviderAgent(data, {}, Request(provider, source, "read"), &imported, &error));
		UAM_ASSERT_EQ(imported.max_workspace_access, std::string("read"));
		const fs::path target = data / "agents" / (id + ".md");
		const std::string stored = uam::io::ReadTextFile(target);
		uam::AgentDefinition edited = imported;
		edited.description = "Edited";
		edited.workspace_access = "write";
		UAM_ASSERT(!uam::AgentDefinitionService::SaveGlobalAgent(data, edited, &error));
		UAM_ASSERT(error.starts_with("workspaceAccess:"));
		UAM_ASSERT_EQ(uam::io::ReadTextFile(target), stored);
		edited.workspace_access = "read";
		UAM_ASSERT(uam::AgentDefinitionService::SaveGlobalAgent(data, edited, &error));
		UAM_ASSERT(Contains(uam::io::ReadTextFile(target), "maxWorkspaceAccess: read"));
	}
	// Unknown tool names do not make a source writable.
	WriteSource(claude, Frontmatter("name: reader\ndescription: Reads\ntools: Read, Grep, BashOutput\n"));
	UAM_ASSERT_EQ(uam::AgentDefinitionService::PreviewProviderAgentImport(uam::provider_ids::kClaudeCli, claude).max_workspace_access, std::string("read"));
}

UAM_TEST(ProviderAgentImportErrorsNameFieldsAndPromptFilesStayInTheirConfig)
{
	TempDir temp("uam-agent-import-named-errors");
	ScopedEnvVar isolated_config("XDG_CONFIG_HOME", (temp.root / "xdg").string());
	const std::string opencode = uam::provider_ids::kOpenCodeCli;
	std::string error;
	const fs::path source = WriteSource(temp.root / "agent.md", Frontmatter("description: d\nmodel: x\ncolor: blue\n"));
	auto request = Request(opencode, source, "read");
	request.acknowledge_ignored_fields = false;
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(error.starts_with("color, model:"));
	request.acknowledge_ignored_fields = true;
	request.skills = {"review", "review"};
	UAM_ASSERT(!uam::AgentDefinitionService::ImportProviderAgent(temp.root / "data", {}, request, nullptr, &error));
	UAM_ASSERT(error.starts_with("skills:"));

	WriteSource(temp.root / "secret.txt", "private\n");
	const fs::path config = WriteSource(temp.root / "project" / "opencode.json",
	    R"({"agent": {"out": {"description": "x", "prompt": "{file:../secret.txt}"}, "in": {"description": "y", "prompt": "{file:./prompts/in.md}"}}})");
	WriteSource(temp.root / "project" / "prompts" / "in.md", "Inside.\n");
	auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "out");
	UAM_ASSERT(!preview.supported);
	UAM_ASSERT(preview.error.starts_with("prompt: the {file:...} reference must name a file inside"));
	preview = uam::AgentDefinitionService::PreviewProviderAgentImport(opencode, config, "in");
	UAM_ASSERT(preview.supported);
	UAM_ASSERT(AnyNoteContains(preview, "prompt: instructions are read from prompts/in.md"));
}
