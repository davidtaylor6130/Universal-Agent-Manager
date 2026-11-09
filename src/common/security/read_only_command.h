#pragma once

#include "common/vcs/vcs_provider_declaration.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uam::command_safety
{
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

using Options = vcs::ReadOnlyCommandOptions;

inline bool Arguments(const std::vector<std::string>& words, std::size_t first, const Options& allowed,
	bool operands = true, bool powershell = false)
{
	// Do not consume a following option as a value: several native options have optional arguments.
	bool paths_only = false;
	unsigned disabled_git_helpers = 0;
	bool internal_diff = false;
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
			continue;
		}
		if (Named(allowed.switches, argument))
		{
			// Only actual parsed switches count: operands and option values never establish this proof.
			if (argument == "--no-ext-diff") disabled_git_helpers |= 1u;
			if (argument == "--no-textconv") disabled_git_helpers |= 2u;
			if (argument == "--internal-diff") internal_diff = true;
			continue;
		}
		const auto equals = argument.find('=');
		const auto name = std::string_view(argument).substr(0, equals);
		if (Named(allowed.values, name))
		{
			if (equals != std::string::npos)
			{
				if (equals + 1 == argument.size()) return false;
			}
			else if (++index == words.size() || words[index].empty() || words[index].front() == '-') return false;
			continue;
		}
		if (powershell || argument.starts_with("--")) return false;
		for (std::size_t offset = 1; offset < argument.size(); ++offset)
		{
			if (allowed.short_switches.find(argument[offset]) != std::string_view::npos) continue;
			if (allowed.short_values.find(argument[offset]) == std::string_view::npos) return false;
			if (offset + 1 == argument.size() && (++index == words.size() || words[index].empty() || words[index].front() == '-')) return false;
			break; // The rest of this token, or the next token, is this option's value.
		}
	}
	return (!allowed.require_git_helper_suppression || disabled_git_helpers == 3u) && (!allowed.require_svn_internal_diff || internal_diff);
}
}

/// <summary>Recognizes supported literal read commands independently of approval risk tiers.</summary>
inline bool IsReadOnlyCommand(std::string_view command)
{
	using namespace read_only_detail;
	std::vector<std::string> words;
	if (!LiteralWords(command, words)) return false;
	const std::string& executable = words.front();
	if (executable == "pwd") return Arguments(words, 1, {"LP", "", "", ""}, false);
	if (executable == "cat") return Arguments(words, 1, {"AbEnstTuv", "", "--number|--number-nonblank|--squeeze-blank|--show-all|--show-ends|--show-tabs|--show-nonprinting", ""});
	if (executable == "ls") return Arguments(words, 1, {"aAbBcCdDfFgGhHiklLmnoOpqQrRsStTuUvwxX1", "", "--all|--almost-all|--directory|--human-readable|--recursive|--reverse|--color=never|--color=auto|--color=always", ""});
	if (executable == "grep") return Arguments(words, 1, {"abcEFGhHiIlLnopqrsRvVwxZz", "efmABC", "--extended-regexp|--fixed-strings|--ignore-case|--line-number|--recursive|--files-with-matches|--files-without-match|--count|--invert-match|--word-regexp|--line-regexp|--quiet|--no-messages|--color=never|--color=auto|--color=always", "--regexp|--file|--max-count|--after-context|--before-context|--context|--include|--exclude|--exclude-dir"});
	if (executable == "head" || executable == "tail") return Arguments(words, 1, {"qv", "nc", "--quiet|--silent|--verbose", "--lines|--bytes"});
	if (executable == "wc") return Arguments(words, 1, {"clLmw", "", "--bytes|--chars|--lines|--max-line-length|--words", ""});
	if (executable == "stat") return Arguments(words, 1, {"lLntx", "cf", "--dereference|--terse|--file-system", "--format|--printf"});
	if (executable == "file") return Arguments(words, 1, {"bcdEhiIkLNrsv", "efFmP", "--brief|--checking-printout|--dereference|--mime|--mime-type|--mime-encoding|--no-buffer|--raw|--special-files|--version", "--exclude|--files-from|--separator|--magic-file|--parameter"});
	if (executable == "which") return Arguments(words, 1, {"as", "", "--all|--skip-alias|--skip-functions|--skip-dot|--skip-tilde|--show-dot|--show-tilde", ""});
	if (executable == "where" || executable == "type") return Arguments(words, 1, {"", "", "", ""});
	if (Lower(executable) == "get-content") return Arguments(words, 1, {"", "", "-raw|-force|-wait|-asbytestream", "-path|-literalpath|-totalcount|-tail|-encoding|-readcount|-delimiter"}, true, true);
	for (const vcs::VcsProviderDeclaration* provider : vcs::kVcsProviderDeclarations)
	{
		if (executable != provider->executable) continue;
		std::size_t subcommand = 1;
		if (!provider->optional_leading_flag.empty() && subcommand < words.size() && words[subcommand] == provider->optional_leading_flag) ++subcommand;
		if (subcommand == words.size()) return false;
		const std::string& action = words[subcommand++];
		for (const vcs::ReadOnlyCommand& rule : provider->read_only_commands)
			if (Named(rule.actions, action)) return Arguments(words, subcommand, rule.options);
		return false;
	}
	return false;
}
}
