#pragma once

#include "common/utils/env_utils.h"
#include "common/vcs/vcs_provider_declaration.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::command_safety
{
/// <summary>Where admitted reads may look, captured when the request arrives.</summary>
struct ReadScope
{
	std::filesystem::path workspace; // Empty admits no command.
	std::filesystem::path cwd;       // Provider-reported working directory; empty means the workspace.
	std::filesystem::path data_root; // UAM's own data stays unreadable unless it holds the workspace.
	bool local = true;               // Remote workspaces can only be checked lexically, without resolving links.
	std::string search_path;         // Executable search path (PATH) captured from UAM's environment.
	std::string path_extensions;     // Windows PATHEXT.
#if defined(_WIN32)
	bool windows_search = true;      // cmd.exe tries the working directory first, with PATHEXT suffixes.
#else
	bool windows_search = false;
#endif
	bool resolve_executables = true; // Admitted executables must resolve to files outside the workspace.
};

/// <summary>Set on every structured provider process so admitted Git reads never start a pager, and locally never a
/// configured filesystem monitor. Appends to any inherited GIT_CONFIG_COUNT entries rather than replacing them.</summary>
inline std::vector<std::pair<std::string, std::string>> ReadCommandEnvironment(bool local)
{
	std::vector<std::pair<std::string, std::string>> environment{{"GIT_PAGER", "cat"}, {"PAGER", "cat"}};
	if (!local) return environment; // The remote host's own GIT_CONFIG_COUNT is unknown here.
	int count = 0;
	if (const std::optional<std::string> inherited = uam::env::GetNonEmptyString("GIT_CONFIG_COUNT"))
	{
		const auto parsed = std::from_chars(inherited->data(), inherited->data() + inherited->size(), count);
		if (parsed.ec != std::errc{} || parsed.ptr != inherited->data() + inherited->size() || count < 0 || count > 1000) return environment;
	}
	environment.insert(environment.end(), {{"GIT_CONFIG_KEY_" + std::to_string(count), "core.fsmonitor"},
	    {"GIT_CONFIG_VALUE_" + std::to_string(count), "false"}, {"GIT_CONFIG_COUNT", std::to_string(count + 1)}});
	return environment;
}

namespace read_only_detail
{
// A deliberately small common literal syntax, not a shell interpreter.
// Reject expansion even inside quotes because the executing shell may differ from this build host.
// Single quotes cannot group whitespace portably (cmd.exe); double-quoted literal paths can.
inline bool LiteralWords(std::string_view command, std::vector<std::string>& words)
{
	if (command.empty() || command.size() > 32768) return false;
	std::string word;
	char quote = 0;
	bool started = false;
	bool closed_quote = false;
	for (const unsigned char value : command)
	{
		const char ch = static_cast<char>(value);
		if ((value < 32 && ch != '\t') || value == 127 ||
			std::string_view("$`\\;|&<>(){}%^!@#").find(ch) != std::string_view::npos) return false;
		if (quote)
		{
			if (ch == quote)
			{
				quote = 0;
				closed_quote = true;
			}
			else
			{
				if (ch == '\'' || ch == '"' || ch == '\t' ||
					(quote == '\'' && (ch == ' ' || value >= 128))) return false;
				word += ch;
			}
			continue;
		}
		if (ch == ' ' || ch == '\t')
		{
			if (started)
			{
				words.push_back(std::move(word));
				word.clear();
			}
			started = false;
			closed_quote = false;
			continue;
		}
		if (value >= 128) return false; // Unicode paths remain available inside double quotes.
		if (closed_quote) return false; // No quoted/unquoted token concatenation.
		if (ch == '\'' || ch == '"')
		{
			if (started) return false;
			quote = ch;
			started = true;
			continue;
		}
		if (std::string_view("*?[],").find(ch) != std::string_view::npos) return false;
		word += ch;
		started = true;
	}
	if (quote) return false;
	if (started) words.push_back(std::move(word));
	return !words.empty() && !words.front().empty();
}

inline bool Named(std::string_view names, std::string_view value)
{
	while (!names.empty())
	{
		const auto end = names.find('|');
		if (names.substr(0, end) == value) return true;
		if (end == std::string_view::npos) break;
		names.remove_prefix(end + 1);
	}
	return false;
}

inline std::string Lower(std::string text)
{
	for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
	return text;
}

// UTF-8 path conversion without path_utils.h, which brings <windows.h> (and its min/max macros) into every includer.
inline std::filesystem::path Utf8Path(std::string_view text)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline std::string Utf8Text(const std::filesystem::path& path)
{
	const std::u8string value = path.generic_u8string();
	return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

// Case-insensitive volumes are the default on Windows and macOS; fold ASCII so case cannot dodge a boundary.
inline std::filesystem::path Folded(const std::filesystem::path& path)
{
#if defined(_WIN32) || defined(__APPLE__)
	return Utf8Path(Lower(Utf8Text(path)));
#else
	return path;
#endif
}

inline std::filesystem::path Resolved(const std::filesystem::path& path, bool local)
{
	if (path.empty()) return {};
	if (!local) return Folded(path.lexically_normal());
	std::error_code error;
	const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error); // Follows existing links.
	return error ? std::filesystem::path{} : Folded(resolved.lexically_normal());
}

inline bool Within(const std::filesystem::path& path, const std::filesystem::path& root)
{
	if (path.empty() || root.empty()) return false;
	const std::filesystem::path relative = path.lexically_relative(root);
	return !relative.empty() && *relative.begin() != "..";
}

// Network operands and words a shell may expand to the home directory never reach the filesystem check.
inline bool Unscoped(std::string_view word)
{
	return word.find("://") != std::string_view::npos || word.starts_with("~") ||
	       word.find("=~") != std::string_view::npos || word.find(":~") != std::string_view::npos;
}

// Only a Windows host resolves drive paths; elsewhere `C:/...` would wrongly look workspace-relative.
inline bool ForeignDrivePath(std::string_view word)
{
#if defined(_WIN32)
	(void)word;
	return false;
#else
	return word.size() > 1 && word[1] == ':' && ((word[0] >= 'A' && word[0] <= 'Z') || (word[0] >= 'a' && word[0] <= 'z'));
#endif
}

inline std::filesystem::path BaseDirectory(const ReadScope& scope)
{
	return scope.cwd.empty() ? scope.workspace : scope.workspace / scope.cwd; // An absolute cwd replaces the workspace.
}

/// <summary>True when an operand resolves inside the workspace and outside UAM's data root.</summary>
/// <summary>The operand's resolved path relative to the workspace when it stays inside the workspace and out of UAM's
/// data root; nothing otherwise.</summary>
inline std::optional<std::filesystem::path> ScopeRelative(std::string_view operand, const ReadScope& scope)
{
	if (Unscoped(operand) || ForeignDrivePath(operand)) return std::nullopt;
	// A colon names a PowerShell provider drive (env:, HKCU:), a Git object (HEAD:path) or a URL; only a Windows drive letter may use one.
	const auto colon = operand.find(':');
	const bool drive = colon == 1 && ((operand[0] >= 'A' && operand[0] <= 'Z') || (operand[0] >= 'a' && operand[0] <= 'z')) && operand.find(':', 2) == std::string_view::npos;
	if (colon != std::string_view::npos && !drive) return std::nullopt;
	std::string text(operand);
	if (!scope.local) std::replace(text.begin(), text.end(), '\\', '/'); // A remote Windows path must not hide its separators lexically.
	const std::filesystem::path workspace = Resolved(scope.workspace, scope.local);
	const std::filesystem::path base = BaseDirectory(scope);
	const std::filesystem::path target = Resolved(text.empty() ? base : base / Utf8Path(text), scope.local);
	if (!Within(target, workspace)) return std::nullopt;
	const std::filesystem::path data_root = Resolved(scope.data_root, scope.local);
	if (Within(target, data_root) && !Within(workspace, data_root)) return std::nullopt;
	return target.lexically_relative(workspace);
}

inline bool InsideScope(std::string_view operand, const ReadScope& scope)
{
	return ScopeRelative(operand, scope).has_value();
}

/// <summary>The program a shell would run for an admitted name must be a file outside the workspace, found through an
/// absolute search entry. A hit from the working directory (cmd.exe), an empty or relative PATH entry, or the workspace
/// itself is refused, as is a name that resolves nowhere. Builtins never search PATH. Remote hosts are not resolved.</summary>
inline bool TrustedExecutable(std::string_view name, const ReadScope& scope)
{
	if (!scope.resolve_executables || !scope.local || name == "pwd" || name == "type" || Lower(std::string(name)) == "get-content") return true;
	const std::filesystem::path workspace = Resolved(scope.workspace, true);
	if (workspace.empty()) return false;
	std::vector<std::pair<std::filesystem::path, bool>> directories; // directory, relative to the working directory
	if (scope.windows_search) directories.emplace_back(std::filesystem::path{}, true);
	const char separator = scope.windows_search ? ';' : ':';
	for (std::string_view rest = scope.search_path;;)
	{
		const auto end = rest.find(separator);
		std::string_view entry = rest.substr(0, end);
		if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') entry = entry.substr(1, entry.size() - 2);
		const std::filesystem::path directory = Utf8Path(entry);
		directories.emplace_back(directory, entry.empty() || !directory.is_absolute());
		if (end == std::string_view::npos) break;
		rest.remove_prefix(end + 1);
	}
	std::vector<std::string> names;
	if (scope.windows_search)
	{
		if (name.find('.') != std::string_view::npos) names.emplace_back(name);
		std::string_view extensions = scope.path_extensions.empty() ? std::string_view(".COM;.EXE;.BAT;.CMD") : std::string_view(scope.path_extensions);
		for (;;)
		{
			const auto end = extensions.find(';');
			if (end != 0 && !extensions.empty()) names.push_back(std::string(name) + std::string(extensions.substr(0, end)));
			if (end == std::string_view::npos) break;
			extensions.remove_prefix(end + 1);
		}
	}
	else names.emplace_back(name);
	const std::filesystem::path base = BaseDirectory(scope);
	constexpr auto executable = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
	for (const auto& [directory, relative] : directories)
	{
		for (const std::string& candidate : names)
		{
			const std::filesystem::path file = (relative ? base / directory : directory) / Utf8Path(candidate);
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(file, error);
			if (error || !std::filesystem::is_regular_file(status)) continue;
			if (!scope.windows_search && (status.permissions() & executable) == std::filesystem::perms::none) continue;
			return !relative && !Within(Resolved(file, true), workspace);
		}
	}
	return false;
}

using Options = vcs::ReadOnlyCommandOptions;

inline bool Satisfied(std::string_view required, const std::vector<std::string>& seen)
{
	while (!required.empty())
	{
		const auto end = required.find('|');
		std::string_view alternatives = required.substr(0, end);
		bool found = false;
		while (!found && !alternatives.empty())
		{
			const auto comma = alternatives.find(',');
			found = std::find(seen.begin(), seen.end(), alternatives.substr(0, comma)) != seen.end();
			alternatives = comma == std::string_view::npos ? std::string_view{} : alternatives.substr(comma + 1);
		}
		if (!found) return false;
		if (end == std::string_view::npos) break;
		required.remove_prefix(end + 1);
	}
	return true;
}

/// <param name="pattern_operand">grep: the first operand is the pattern unless -e/-f supplies one.</param>
inline bool Arguments(const std::vector<std::string>& words, std::size_t first, const Options& allowed, const ReadScope& scope,
	bool operands = true, bool powershell = false, bool pattern_operand = false)
{
	// Do not consume a following option as a value: several native options have optional arguments.
	bool paths_only = false;
	bool explicit_pattern = false;
	std::vector<std::string> seen; // Only parsed switches: operands and option values never satisfy a requirement.
	std::vector<std::string_view> positional;
	const auto value_ok = [&](std::string_view name, std::string_view value)
	{
		if (pattern_operand && Named("-e|--regexp|-f|--file", name)) explicit_pattern = true;
		if (Named(allowed.path_values, name)) return InsideScope(value, scope);
		return !Unscoped(value);
	};
	for (std::size_t index = first; index < words.size(); ++index)
	{
		const std::string argument = powershell ? Lower(words[index]) : words[index];
		if (!paths_only && argument == "--")
		{
			paths_only = true;
			continue;
		}
		if (paths_only || argument.empty() || argument.front() != '-' || argument == "-")
		{
			if (!operands) return false;
			positional.push_back(words[index]);
			continue;
		}
		if (Named(allowed.switches, argument))
		{
			seen.push_back(argument);
			continue;
		}
		const auto equals = argument.find('=');
		const auto name = std::string_view(argument).substr(0, equals);
		if (Named(allowed.values, name))
		{
			if (equals != std::string::npos)
			{
				if (equals + 1 == argument.size() || !value_ok(name, std::string_view(words[index]).substr(equals + 1))) return false;
			}
			else if (++index == words.size() || words[index].empty() || words[index].front() == '-' || !value_ok(name, words[index])) return false;
			continue;
		}
		if (powershell || argument.starts_with("--")) return false;
		for (std::size_t offset = 1; offset < argument.size(); ++offset)
		{
			if (allowed.short_switches.find(argument[offset]) != std::string_view::npos) continue;
			if (allowed.short_values.find(argument[offset]) == std::string_view::npos) return false;
			const std::string short_name{'-', argument[offset]};
			if (offset + 1 == argument.size())
			{
				if (++index == words.size() || words[index].empty() || words[index].front() == '-' || !value_ok(short_name, words[index])) return false;
			}
			else if (!value_ok(short_name, std::string_view(argument).substr(offset + 1))) return false;
			break; // The rest of this token, or the next token, is this option's value.
		}
	}
	for (std::size_t index = (pattern_operand && !explicit_pattern) || allowed.first_operand_is_name ? 1 : 0; index < positional.size(); ++index)
		if (!InsideScope(positional[index], scope)) return false;
	return Satisfied(allowed.required, seen) && (scope.cwd.empty() || InsideScope("", scope));
}
}

/// <summary>True when a provider-named path resolves inside the read scope's workspace and outside UAM's data root.</summary>
inline bool IsInsideReadScope(std::string_view path, const ReadScope& scope)
{
	return read_only_detail::InsideScope(path, scope);
}

/// <summary>The resolved, case-folded path relative to the workspace (links followed locally) when it is inside the read scope.</summary>
inline std::optional<std::filesystem::path> ReadScopeRelativePath(std::string_view path, const ReadScope& scope)
{
	return read_only_detail::ScopeRelative(path, scope);
}

/// <summary>Recognizes supported literal read commands whose every path operand stays inside <paramref name="scope"/>.</summary>
inline bool IsReadOnlyCommand(std::string_view command, const ReadScope& scope)
{
	using namespace read_only_detail;
	std::vector<std::string> words;
	if (scope.workspace.empty() || !LiteralWords(command, words) || !TrustedExecutable(words.front(), scope)) return false;
	const std::string& executable = words.front();
	if (executable == "pwd") return Arguments(words, 1, {"LP", "", "", ""}, scope, false);
	if (executable == "cat") return Arguments(words, 1, {"AbEnstTuv", "", "--number|--number-nonblank|--squeeze-blank|--show-all|--show-ends|--show-tabs|--show-nonprinting", ""}, scope);
	// No symlink-following recursion (ls -L, grep -R): a link inside the workspace must not list or read outside it.
	if (executable == "ls") return Arguments(words, 1, {"aAbBcCdDfFgGhHiklmnoOpqQrRsStTuUvwxX1", "", "--all|--almost-all|--directory|--human-readable|--recursive|--reverse|--color=never|--color=auto|--color=always", ""}, scope);
	if (executable == "grep") return Arguments(words, 1, {"abcEFGhHiIlLnopqrsvVwxZz", "efmABC", "--extended-regexp|--fixed-strings|--ignore-case|--line-number|--recursive|--files-with-matches|--files-without-match|--count|--invert-match|--word-regexp|--line-regexp|--quiet|--no-messages|--color=never|--color=auto|--color=always", "--regexp|--file|--max-count|--after-context|--before-context|--context|--include|--exclude|--exclude-dir", {}, "-f|--file"}, scope, true, false, true);
	if (executable == "head" || executable == "tail") return Arguments(words, 1, {"qv", "nc", "--quiet|--silent|--verbose", "--lines|--bytes"}, scope);
	if (executable == "wc") return Arguments(words, 1, {"clLmw", "", "--bytes|--chars|--lines|--max-line-length|--words", ""}, scope);
	if (executable == "stat") return Arguments(words, 1, {"lLntx", "cf", "--dereference|--terse|--file-system", "--format|--printf"}, scope);
	if (executable == "file") return Arguments(words, 1, {"bcdEhiIkLNrsv", "efFmP", "--brief|--checking-printout|--dereference|--mime|--mime-type|--mime-encoding|--no-buffer|--raw|--special-files|--version", "--exclude|--files-from|--separator|--magic-file|--parameter", {}, "-f|--files-from|-m|--magic-file"}, scope);
	if (executable == "which") return Arguments(words, 1, {"as", "", "--all|--skip-alias|--skip-functions|--skip-dot|--skip-tilde|--show-dot|--show-tilde", ""}, scope);
	if (executable == "where" || executable == "type") return Arguments(words, 1, {"", "", "", ""}, scope);
	if (Lower(executable) == "get-content") return Arguments(words, 1, {"", "", "-raw|-force|-wait|-asbytestream", "-path|-literalpath|-totalcount|-tail|-encoding|-readcount|-delimiter", {}, "-path|-literalpath"}, scope, true, true);
	for (const vcs::VcsProviderDeclaration* provider : vcs::kVcsProviderDeclarations)
	{
		if (executable != provider->executable) continue;
		std::size_t subcommand = 1;
		if (!provider->optional_leading_flag.empty() && subcommand < words.size() && words[subcommand] == provider->optional_leading_flag) ++subcommand;
		if (subcommand == words.size()) return false;
		const std::string& action = words[subcommand++];
		for (const vcs::ReadOnlyCommand& rule : provider->read_only_commands)
			if (Named(rule.actions, action)) return Arguments(words, subcommand, rule.options, scope);
		return false;
	}
	return false;
}
}
