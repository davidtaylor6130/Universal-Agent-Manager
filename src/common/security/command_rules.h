#pragma once

// User allow/ask/deny rules for permission requests, evaluated locally before any AI review.
//
// Rules only narrow: a deny rule answers with a one-time rejection, an ask rule keeps the request for the
// user (even under YOLO), and an allow rule can only stand in for the AI reviewer's own one-time approval
// when the caller says that approval would have been permitted (Context::allow_permitted). Nothing here can
// approve a request outside the provider ceilings or the high-risk refusals; the caller owns that gate.
//
// Kept free of JSON, provider and platform types so it can be tested in isolation.

#include <algorithm>
#include <initializer_list>
#include <array>
#include <bitset>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uam::command_rules
{

enum class Action
{
	Allow,
	Ask,
	Deny,
};

/// <summary>What a rule targets. Tool kinds come from the provider's structured kind field, never display text.</summary>
enum class Tool
{
	Any,      // Every request (deny/ask only).
	Command,  // Shell commands, matched by pattern against the typed native command.
	ReadOnly, // Commands the literal read filter admits and read/search tools inside the workspace.
	Edit,     // File changes.
	Fetch,    // Web fetches and searches (deny/ask only).
	Other,    // MCP and other tools UAM cannot classify (deny/ask only).
};

/// <summary>The structured category of one pending request.</summary>
enum class Category
{
	Command,
	Read,
	Edit,
	Fetch,
	Other,
};

struct Rule
{
	std::string id;
	Action action = Action::Ask;
	Tool tool = Tool::Command;
	std::string pattern;     // Command pattern; only used with Tool::Command.
	std::string provider_id; // Empty: every provider.
	std::string workspace;   // Empty: every workspace; otherwise this folder and everything inside it.
	bool enabled = true;
};

struct Context
{
	Category category = Category::Other;
	std::string_view command;  // Typed native command; empty when the provider supplied none.
	bool read_only = false;    // Admitted by the literal read filter or a read tool inside the read scope.
	std::string_view provider_id;
	std::string_view workspace;
	// Other folders the chat works for, such as the source folder of a worktree; a workspace rule matches any.
	std::string_view source_workspace;
	std::string_view folder_workspace;
	// Settings tester only: report scoped rules as if their provider and workspace applied.
	bool ignore_scope = false;
	// The hidden advanced override (settings key permission_remote_commits_without_approval). Off by default:
	// pushes and other remote commits then wait for the user under every permission level, YOLO included.
	bool remote_commit_override = false;
	// The caller's ceiling: true only when the AI reviewer itself could have approved this request once
	// (AI Review tier, reviewer-eligible, within Plan/captured ceilings, not high risk, category checks passed).
	bool allow_permitted = false;
};

enum class Outcome
{
	NoMatch,  // Continue with the normal permission flow (AI review when it is on).
	Approve,  // An allow rule replaced the AI reviewer; approve once.
	Deny,     // Reject once.
	AskUser,  // Leave for the user; no automatic approval or AI review.
};

struct Decision
{
	Outcome outcome = Outcome::NoMatch;
	std::string rule_id;
	std::string reason;
	// An allow rule matched but the ceiling did not permit automatic approval; the normal flow continues.
	bool allow_blocked_by_ceiling = false;
	// The built-in remote-commit rule holds the request (a push, merge or SVN commit waits for the user).
	bool remote_commit = false;
};

inline constexpr std::size_t kMaxRules = 200;
/// <summary>Id reported for the built-in rule that holds pushes and other remote commits for the user.</summary>
inline constexpr std::string_view kRemoteCommitRuleId = "builtin-remote-commits";
inline constexpr std::string_view kRemoteCommitNotice = "Pushes, merges and other remote commits always wait for your approval.";
inline constexpr std::size_t kMaxPatternBytes = 512;
inline constexpr std::size_t kMaxFieldBytes = 1024;
inline constexpr std::size_t kMaxCommandBytes = 64 * 1024;
inline constexpr int kMaxNesting = 4;
// Bounds on agent-controlled input: past these the command is left for the user rather than analysed further,
// so matching stays linear in the command and the pattern.
inline constexpr std::size_t kMaxWordsPerSegment = 128;
inline constexpr std::size_t kMaxTotalWords = 4096;
inline constexpr std::size_t kMaxInvocations = 512;
inline constexpr std::size_t kMaxNestedBodies = 64;
inline constexpr std::size_t kMaxGlobWordBytes = 128;

inline std::string_view ActionName(Action action)
{
	return action == Action::Allow ? "allow" : action == Action::Deny ? "deny" : "ask";
}

inline std::optional<Action> ParseAction(std::string_view value)
{
	if (value == "allow") return Action::Allow;
	if (value == "ask") return Action::Ask;
	if (value == "deny") return Action::Deny;
	return std::nullopt;
}

inline std::string_view ToolName(Tool tool)
{
	switch (tool)
	{
	case Tool::Any: return "any";
	case Tool::Command: return "command";
	case Tool::ReadOnly: return "readOnly";
	case Tool::Edit: return "edit";
	case Tool::Fetch: return "fetch";
	case Tool::Other: return "other";
	}
	return "command";
}

inline std::optional<Tool> ParseTool(std::string_view value)
{
	for (Tool tool : {Tool::Any, Tool::Command, Tool::ReadOnly, Tool::Edit, Tool::Fetch, Tool::Other})
		if (ToolName(tool) == value) return tool;
	return std::nullopt;
}

/// <summary>Maps a provider's structured kind (and whether its input names a URL) to a category.</summary>
inline Category CategoryForKind(std::string_view kind, bool uses_network)
{
	std::string lower;
	for (char ch : kind)
		if (ch != ' ' && ch != '\t') lower.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch);
	// A typed command stays a command even when it names a URL, so command rules still see curl and wget.
	if (lower == "commandexecution" || lower == "execute") return Category::Command;
	if (uses_network || lower == "fetch") return Category::Fetch;
	if (lower == "read" || lower == "search" || lower == "inspect" || lower == "list" || lower == "query" || lower == "find") return Category::Read;
	if (lower == "filechange" || lower == "edit" || lower == "write" || lower == "create" || lower == "delete" || lower == "move") return Category::Edit;
	return Category::Other;
}

namespace detail
{
	inline char Lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }

	inline bool Space(char ch) { return ch == ' ' || ch == '\t'; }

	/// <summary>Glob over one word: `*` any run, `?` one character, `[...]` a class (`[!...]` negated).</summary>
	inline bool GlobMatch(std::string_view pattern, std::string_view text, bool fold_case = false)
	{
		std::size_t p = 0, t = 0, star_p = std::string_view::npos, star_t = 0;
		const auto eq = [&](char a, char b) { return fold_case ? Lower(a) == Lower(b) : a == b; };
		while (t < text.size())
		{
			if (p < pattern.size() && pattern[p] == '*')
			{
				star_p = p++;
				star_t = t;
				continue;
			}
			if (p < pattern.size() && pattern[p] == '[')
			{
				const std::size_t close = pattern.find(']', p + 2);
				if (close != std::string_view::npos)
				{
					std::size_t q = p + 1;
					const bool negate = pattern[q] == '!' || pattern[q] == '^';
					if (negate) ++q;
					bool found = false;
					for (; q < close; ++q)
					{
						if (q + 2 < close && pattern[q + 1] == '-')
						{
							const char low = fold_case ? Lower(pattern[q]) : pattern[q];
							const char high = fold_case ? Lower(pattern[q + 2]) : pattern[q + 2];
							const char value = fold_case ? Lower(text[t]) : text[t];
							found = found || (value >= low && value <= high);
							q += 2;
						}
						else found = found || eq(pattern[q], text[t]);
					}
					if (found != negate)
					{
						p = close + 1;
						++t;
						continue;
					}
				}
				else if (eq('[', text[t]))
				{
					++p;
					++t;
					continue;
				}
			}
			else if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], text[t])))
			{
				++p;
				++t;
				continue;
			}
			if (star_p == std::string_view::npos) return false;
			p = star_p + 1;
			t = ++star_t;
		}
		while (p < pattern.size() && pattern[p] == '*') ++p;
		return p == pattern.size();
	}

	struct Word
	{
		std::string text;
		bool literal = true;  // No unquoted glob, tilde or brace expansion.
		bool dynamic = false; // Contains a parameter, command or ANSI-C expansion: its run-time value is unknown.
		bool quoted = false;
		bool brace_list = false; // Holds an unquoted {a,b} or {1..3} list: the shell turns it into several words.
		std::vector<unsigned char> meta; // Per character of text: kGlobMark, kDynamicMark or 0 for plain text.
	};

	inline constexpr unsigned char kGlobMark = 1;    // Unquoted *, ?, [, ], { or }.
	inline constexpr unsigned char kDynamicMark = 2; // Part of $VAR, ${...}, $'...', %VAR% or a substitution.

	/// <summary>Whether the word holds an unquoted brace list with a comma or a `..` range at its top level.</summary>
	inline bool HasBraceList(const Word& word)
	{
		const std::string& text = word.text;
		for (std::size_t i = 0; i < text.size(); ++i)
		{
			if (text[i] != '{' || word.meta[i] != kGlobMark) continue;
			int depth = 0;
			for (std::size_t j = i; j < text.size(); ++j)
			{
				if (word.meta[j] == kGlobMark && text[j] == '{') ++depth;
				else if (word.meta[j] == kGlobMark && text[j] == '}' && --depth == 0) break;
				else if (depth == 1 && (text[j] == ',' || (text[j] == '.' && j + 1 < text.size() && text[j + 1] == '.'))) return true;
			}
		}
		return false;
	}

	struct Lexed
	{
		std::vector<std::vector<Word>> segments; // Simple commands split at control operators.
		std::vector<std::string> nested;         // Command and process substitution bodies.
		bool ok = true;                          // Quotes and substitutions were closed.
		bool compound = false;                   // Operators, redirection, subshells or substitutions appeared.
		bool dynamic = false;                    // Any expansion whose value is only known at run time.
	};

	/// <summary>Returns the index just past the `)` closing a `$(` or `<(` body that starts at `begin`,
	/// skipping quoted text, or npos when it is unterminated.</summary>
	inline std::size_t SubstitutionEnd(std::string_view text, std::size_t begin)
	{
		int depth = 1;
		for (std::size_t i = begin; i < text.size(); ++i)
		{
			const char ch = text[i];
			if (ch == '\\') { ++i; continue; }
			if (ch == '\'')
			{
				const std::size_t close = text.find('\'', i + 1);
				if (close == std::string_view::npos) return std::string_view::npos;
				i = close;
				continue;
			}
			if (ch == '"')
			{
				std::size_t j = i + 1;
				for (; j < text.size() && text[j] != '"'; ++j)
					if (text[j] == '\\') ++j;
				if (j >= text.size()) return std::string_view::npos;
				i = j;
				continue;
			}
			if (ch == '(') ++depth;
			else if (ch == ')' && --depth == 0) return i + 1;
		}
		return std::string_view::npos;
	}

	inline std::size_t BacktickEnd(std::string_view text, std::size_t begin)
	{
		for (std::size_t i = begin; i < text.size(); ++i)
		{
			if (text[i] == '\\') { ++i; continue; }
			if (text[i] == '`') return i + 1;
		}
		return std::string_view::npos;
	}

	/// <summary>POSIX-shell-shaped lexer. It is deliberately conservative: anything it cannot read exactly marks
	/// the command compound or dynamic, which stops allow rules and makes deny evaluation over-approximate.</summary>
	inline Lexed Lex(std::string_view text)
	{
		Lexed out;
		std::vector<Word> segment;
		Word word;
		bool in_word = false;
		bool skip_next_word = false; // The word after a redirection operator names a file, not an argument.
		const auto put = [&](char value, unsigned char meta = 0)
		{
			word.text.push_back(value);
			word.meta.push_back(meta);
		};
		// Marks a parameter expansion starting at text[at] == '$' as one dynamic region; returns the index after it.
		const auto dynamic_name = [&](std::size_t at, bool in_double_quotes)
		{
			word.dynamic = out.dynamic = true;
			put('$', kDynamicMark);
			std::size_t j = at + 1;
			if (j < text.size() && (text[j] == '{' || (text[j] == '\'' && !in_double_quotes)))
			{
				const char close = text[j] == '{' ? '}' : '\'';
				const std::size_t end = text.find(close, j + 1);
				const std::size_t stop = end == std::string_view::npos ? text.size() : end + 1;
				for (; j < stop; ++j) put(text[j], kDynamicMark);
				return j;
			}
			if (j < text.size() && !((text[j] >= 'a' && text[j] <= 'z') || (text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= '0' && text[j] <= '9') || text[j] == '_'))
			{
				if (text[j] != '"' && !Space(text[j])) put(text[j++], kDynamicMark); // $1-style specials: $?, $@, $#.
				return j;
			}
			while (j < text.size() && ((text[j] >= 'a' && text[j] <= 'z') || (text[j] >= 'A' && text[j] <= 'Z') || (text[j] >= '0' && text[j] <= '9') || text[j] == '_')) put(text[j++], kDynamicMark);
			return j;
		};
		// cmd.exe %VAR%: from '%' through the next '%' in the same word, as one dynamic region.
		const auto percent_name = [&](std::size_t at)
		{
			word.dynamic = out.dynamic = true;
			std::size_t end = at + 1;
			while (end < text.size() && text[end] != '%' && !Space(text[end]) && text[end] != '"' && text[end] != '\'') ++end;
			const std::size_t stop = end < text.size() && text[end] == '%' ? end + 1 : at + 1;
			for (std::size_t j = at; j < stop; ++j) put(text[j], kDynamicMark);
			return stop;
		};
		const auto put_text = [&](std::string_view value)
		{
			for (char c : value) put(c);
		};
		const auto end_word = [&]
		{
			if (!in_word) return;
			if (HasBraceList(word)) word.literal = false, word.brace_list = true;
			if (skip_next_word) skip_next_word = false;
			else segment.push_back(std::move(word));
			word = Word{};
			in_word = false;
		};
		const auto end_segment = [&]
		{
			end_word();
			if (!segment.empty()) out.segments.push_back(std::move(segment));
			segment.clear();
		};
		const auto substitution = [&](std::size_t body_begin, std::size_t end, std::size_t trim_tail)
		{
			if (end == std::string_view::npos)
			{
				out.ok = false;
				out.nested.emplace_back(text.substr(body_begin));
				return text.size();
			}
			out.nested.emplace_back(text.substr(body_begin, end - body_begin - trim_tail));
			out.compound = out.dynamic = true;
			word.dynamic = true;
			put('$', kDynamicMark); // Stands for the substitution's output inside the word.
			in_word = true;
			return end;
		};
		std::size_t i = 0;
		while (i < text.size())
		{
			const char ch = text[i];
			if (ch == '\\')
			{
				if (i + 1 >= text.size()) { ++i; continue; }
				if (text[i + 1] == '\n') { i += 2; continue; }
				if (text[i + 1] == '\r' && i + 2 < text.size() && text[i + 2] == '\n') { i += 3; continue; }
				put(text[i + 1]);
				in_word = true;
				i += 2;
				continue;
			}
			if (ch == '\'')
			{
				const std::size_t close = text.find('\'', i + 1);
				if (close == std::string_view::npos) { out.ok = false; put_text(text.substr(i + 1)); in_word = true; break; }
				put_text(text.substr(i + 1, close - i - 1));
				word.quoted = in_word = true;
				i = close + 1;
				continue;
			}
			if (ch == '"')
			{
				word.quoted = in_word = true;
				std::size_t j = i + 1;
				bool closed = false;
				while (j < text.size())
				{
					const char inner = text[j];
					if (inner == '"') { closed = true; ++j; break; }
					if (inner == '\\' && j + 1 < text.size())
					{
						const char next = text[j + 1];
						if (next == '"' || next == '\\' || next == '$' || next == '`') put(next);
						else if (next != '\n') { put('\\'); put(next); }
						j += 2;
						continue;
					}
					if (inner == '`')
					{
						j = substitution(j + 1, BacktickEnd(text, j + 1), 1);
						continue;
					}
					if (inner == '$' && j + 1 < text.size() && text[j + 1] == '(')
					{
						j = substitution(j + 2, SubstitutionEnd(text, j + 2), 1);
						continue;
					}
					if (inner == '$' && j + 1 < text.size() && text[j + 1] != '"')
					{
						j = dynamic_name(j, true);
						continue;
					}
					if (inner == '%')
					{
						j = percent_name(j);
						continue;
					}
					put(inner);
					++j;
				}
				if (!closed) { out.ok = false; break; }
				i = j;
				continue;
			}
			if (ch == '`')
			{
				i = substitution(i + 1, BacktickEnd(text, i + 1), 1);
				continue;
			}
			if (ch == '$')
			{
				if (i + 1 < text.size() && text[i + 1] == '(')
				{
					i = substitution(i + 2, SubstitutionEnd(text, i + 2), 1);
					continue;
				}
				// $VAR, ${...}, $'...' (ANSI-C), $"..." (locale) and $1: the value is only known at run time.
				in_word = true;
				i = dynamic_name(i, false);
				continue;
			}
			// '#' is deliberately not a comment: cmd.exe has none, so text after it can still run there.
			if (ch == '%')
			{
				// cmd.exe expands %VAR% before it parses operators, so the value is only known at run time.
				in_word = true;
				i = percent_name(i);
				continue;
			}
			if (Space(ch))
			{
				end_word();
				++i;
				continue;
			}
			if (ch == '\n' || ch == '\r' || ch == ';' || ch == '|' || ch == '(' || ch == ')')
			{
				end_segment();
				out.compound = true;
				++i;
				continue;
			}
			if (ch == '&')
			{
				if (i + 1 < text.size() && text[i + 1] == '>')
				{
					end_word();
					out.compound = true;
					i += 2;
					while (i < text.size() && text[i] == '>') ++i;
					skip_next_word = true;
					continue;
				}
				end_segment();
				out.compound = true;
				++i;
				continue;
			}
			if (ch == '<' || ch == '>')
			{
				// A file-descriptor number attached to the operator (2>file) is part of the redirection.
				if (in_word && !word.quoted && !word.text.empty() &&
				    std::all_of(word.text.begin(), word.text.end(), [](char c) { return c >= '0' && c <= '9'; }))
				{
					word = Word{};
					in_word = false;
				}
				end_word();
				out.compound = true;
				++i;
				if (i < text.size() && text[i] == '(')
				{
					// Process substitution: <(...) or >(...).
					i = substitution(i + 1, SubstitutionEnd(text, i + 1), 1);
					word = Word{};
					in_word = false;
					continue;
				}
				while (i < text.size() && (text[i] == '<' || text[i] == '>' || text[i] == '&' || text[i] == '|')) ++i;
				// >&2 and <&- name a descriptor, not a file.
				if (i < text.size() && (text[i] == '-' || (text[i] >= '0' && text[i] <= '9')) && i > 0 && text[i - 1] == '&')
				{
					while (i < text.size() && !Space(text[i]) && text[i] != ';' && text[i] != '|' && text[i] != '&' && text[i] != '\n') ++i;
					continue;
				}
				skip_next_word = true;
				continue;
			}
			// '~' stays literal text: the home directory is a fixed path, and patterns such as `*.ssh/*` match it as written.
			// Braces only expand as a list (checked when the word ends), so `{}` and `@{1}` stay literal.
			const bool glob = ch == '*' || ch == '?' || ch == '[' || ch == ']';
			if (glob) word.literal = false;
			put(ch, glob || ch == '{' || ch == '}' ? kGlobMark : 0);
			in_word = true;
			++i;
		}
		end_segment();
		return out;
	}

	inline Lexed LexPattern(std::string_view pattern)
	{
		// Patterns use the same quoting so a rule can name an argument with spaces; glob characters stay literal
		// text here and are interpreted by GlobMatch.
		return Lex(pattern);
	}

	/// <summary>The program name a shell looks up: its last path component, lower-cased, without .exe.</summary>
	inline std::string ProgramName(std::string_view word)
	{
		if (const std::size_t slash = word.find_last_of("/\\"); slash != std::string_view::npos) word.remove_prefix(slash + 1);
		std::string name;
		for (char ch : word) name.push_back(Lower(ch));
		for (std::string_view extension : {".exe", ".cmd", ".bat", ".com", ".ps1"})
			if (name.size() > extension.size() && name.ends_with(extension)) { name.resize(name.size() - extension.size()); break; }
		return name;
	}

	inline bool Assignment(const Word& word)
	{
		const std::size_t equals = word.text.find('=');
		if (equals == std::string::npos || equals == 0) return false;
		const char first = word.text.front();
		if (first >= '0' && first <= '9') return false;
		return std::all_of(word.text.begin(), word.text.begin() + static_cast<std::ptrdiff_t>(equals),
		    [](char ch) { return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); });
	}

	/// <summary>Words that run the program named after them (with their own options first).</summary>
	inline bool Wrapper(std::string_view name)
	{
		constexpr std::string_view wrappers[] = {
		    "command", "builtin", "exec", "time", "nice", "ionice", "timeout", "gtimeout", "stdbuf", "caffeinate", "chrt", "taskset", "unbuffer",
		    "nohup", "setsid", "sudo", "doas", "env", "xargs", "watch", "strace", "ltrace", "then", "do", "else", "elif", "if", "while", "until",
		    "!", "{", "}", "start", "start-process", "runas", "flock", "chroot", "systemd-run", "script", "busybox", "nsenter", "unshare", "firejail", "parallel", "call", "wsl"};
		return std::find(std::begin(wrappers), std::end(wrappers), name) != std::end(wrappers);
	}

	/// <summary>Wrapper options whose value is the next word.</summary>
	inline bool WrapperOptionTakesValue(std::string_view wrapper, std::string_view option)
	{
		if (wrapper == "doas") return option == "-u" || option == "-C";
		if (wrapper == "sudo") return option == "-u" || option == "-g" || option == "-C" || option == "-D" || option == "-h" || option == "-p" || option == "-r" || option == "-t" || option == "-U" || option == "-T";
		if (wrapper == "timeout" || wrapper == "gtimeout") return option == "-s" || option == "-k" || option == "--signal" || option == "--kill-after";
		if (wrapper == "nice") return option == "-n" || option == "--adjustment";
		if (wrapper == "ionice") return option == "-c" || option == "-n" || option == "-p" || option == "-P" || option == "-u";
		if (wrapper == "env") return option == "-u" || option == "-C" || option == "--unset" || option == "--chdir";
		if (wrapper == "xargs") return option == "-I" || option == "-L" || option == "-n" || option == "-P" || option == "-d" || option == "-E" || option == "-s" || option == "-a" || option == "--arg-file" || option == "--delimiter" || option == "--max-args" || option == "--max-procs" || option == "--replace";
		if (wrapper == "stdbuf") return option == "-i" || option == "-o" || option == "-e";
		if (wrapper == "watch") return option == "-n" || option == "--interval" || option == "-d";
		if (wrapper == "chroot" || wrapper == "flock" || wrapper == "runas") return false;
		if (wrapper == "start-process")
		{
			std::string lower;
			for (char ch : option) lower.push_back(Lower(ch));
			return lower == "-argumentlist" || lower == "-workingdirectory";
		}
		return false;
	}

	inline bool ShellProgram(std::string_view name)
	{
		constexpr std::string_view shells[] = {"bash", "sh", "zsh", "dash", "ksh", "mksh", "csh", "tcsh", "fish", "ash", "busybox", "cmd", "powershell", "pwsh", "wsl", "su", "eval", "script", "iex", "invoke-expression"};
		return std::find(std::begin(shells), std::end(shells), name) != std::end(shells);
	}

	/// <summary>Options after which a shell reads a command string: -c, combined -lc/-ec, /c, /k, -Command.</summary>
	inline bool CommandStringOption(std::string_view word)
	{
		std::string lower;
		for (char ch : word) lower.push_back(Lower(ch));
		if (lower == "/c" || lower == "/k" || lower == "/r" || lower == "-command" || lower == "-c" || lower == "--command" || lower == "-encodedcommand") return true;
		return lower.size() > 1 && lower[0] == '-' && lower[1] != '-' && lower.find('c') != std::string::npos &&
		       std::all_of(lower.begin() + 1, lower.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; });
	}

	struct Invocation
	{
		std::size_t segment = 0; // Index into CommandView::segments.
		std::size_t start = 0;   // Word where the program starts.
	};

	struct CommandView
	{
		std::vector<std::vector<Word>> segments;  // Simple commands from every lexing pass and nested body.
		std::vector<Invocation> invocations;      // Each starts at a program word; ranges, never copies.
		std::vector<std::string> nested;          // Command strings run by shells, eval and substitutions.
		std::size_t total_words = 0;
		std::size_t nested_bodies = 0;
		bool unresolved = false;                  // A program word is computed at run time, or the text is too
		                                          // large or malformed to analyse completely.
	};

	inline void AddInvocation(CommandView& view, std::size_t segment, std::size_t start)
	{
		if (view.invocations.size() >= kMaxInvocations) { view.unresolved = true; return; }
		view.invocations.push_back({segment, start});
	}

	inline void AddNested(CommandView& view, std::string body, int depth)
	{
		if (depth >= kMaxNesting || view.nested_bodies >= kMaxNestedBodies) { view.unresolved = true; return; }
		++view.nested_bodies;
		view.nested.push_back(std::move(body));
	}

	inline std::string LowerText(std::string_view text)
	{
		std::string lower;
		for (char ch : text) lower.push_back(Lower(ch));
		return lower;
	}

	inline bool OneOf(std::string_view value, std::initializer_list<std::string_view> list)
	{
		return std::find(list.begin(), list.end(), value) != list.end();
	}

	/// <summary>Git subcommands that only change the local repository or read from a remote. Any other
	/// subcommand may be an alias from the user's Git configuration, which could push.</summary>
	inline bool LocalGitSubcommand(std::string_view name)
	{
		return OneOf(name, {"add", "am", "apply", "archive", "bisect", "blame", "branch", "bundle", "cat-file", "check-ignore", "checkout", "cherry",
		    "cherry-pick", "clean", "clone", "commit", "config", "count-objects", "describe", "diff", "diff-files", "diff-index", "diff-tree", "difftool",
		    "fetch", "for-each-ref", "format-patch", "fsck", "gc", "grep", "help", "init", "log", "ls-files", "ls-remote", "ls-tree", "maintenance", "merge",
		    "merge-base", "mergetool", "mv", "notes", "pull", "range-diff", "rebase", "reflog", "remote", "repack", "replace", "reset", "restore", "rev-list",
		    "rev-parse", "revert", "rm", "shortlog", "show", "show-ref", "sparse-checkout", "stash", "status", "submodule", "switch", "symbolic-ref", "tag",
		    "update-index", "update-ref", "var", "verify-commit", "verify-tag", "version", "whatchanged", "worktree", "write-tree", "stage", "show-branch", "prune",
		    "annotate", "hash-object", "rerere", "filter-branch", "commit-graph", "name-rev", "check-attr", "pack-refs", "read-tree", "merge-tree",
		    "interpret-trailers", "request-pull", "gui", "citool", "fast-export", "fast-import", "mailinfo", "mailsplit", "check-ref-format", "multi-pack-index",
		    "prune-packed", "verify-pack", "index-pack", "pack-objects", "unpack-objects", "checkout-index", "merge-file", "patch-id", "column",
		    "update-server-info", "check-mailmap", "credential-cache", "get-tar-commit-id", "ls-remote", "mktag", "mktree", "replay", "restore", "scalar",
		    "shell", "stripspace", "unpack-file", "diagnose", "bugreport", "init-db", "instaweb", "range-diff", "send-email"});
	}

	/// <summary>Non-option words after `start`, skipping the values of the listed options.</summary>
	inline std::vector<std::size_t> Operands(const std::vector<Word>& words, std::size_t start, std::initializer_list<std::string_view> valued_options)
	{
		std::vector<std::size_t> operands;
		for (std::size_t i = start; i < words.size(); ++i)
		{
			const std::string_view text = words[i].text;
			if (text == "--") { for (++i; i < words.size(); ++i) operands.push_back(i); break; }
			if (text.size() > 1 && text.starts_with('-'))
			{
				if (text.find('=') == std::string_view::npos && OneOf(text, valued_options)) ++i;
				continue;
			}
			operands.push_back(i);
		}
		return operands;
	}

	/// <summary>Git's operands after `start`, skipping the values of its global options.</summary>
	inline std::vector<std::size_t> GitOperands(const std::vector<Word>& words, std::size_t start)
	{
		return Operands(words, start, {"-C", "-c", "--git-dir", "--work-tree", "--namespace", "--exec-path", "--config-env", "--super-prefix", "--attr-source"});
	}

	/// <summary>Everything a program starting at words[i] could run: the program itself, shell -c, cmd /c,
	/// PowerShell -Command, eval and iex strings, find -exec targets and, after a wrapper such as sudo, env,
	/// xargs or timeout, every later word as a possible program (so unknown wrapper options cannot hide it).
	/// Over-approximates on purpose (deny side); `visited` keeps it linear in the segment's words.</summary>
	inline void CollectFrom(std::size_t segment_index, std::size_t i, CommandView& view, int depth, std::vector<char>& visited)
	{
		const std::vector<Word>& words = view.segments[segment_index];
		while (i < words.size() && Assignment(words[i])) ++i;
		if (i >= words.size() || visited[i]) return;
		visited[i] = 1;
		// A program computed at run time ($X, %X%, a glob or a brace list) cannot be checked against rules.
		if (words[i].dynamic || !words[i].literal) view.unresolved = true;
		AddInvocation(view, segment_index, i);
		const std::string name = ProgramName(words[i].text);
		const auto rest_of_line = [&](std::size_t from, std::string first)
		{
			for (std::size_t k = from; k < words.size(); ++k) first += " " + words[k].text;
			return first;
		};
		if (name == "invoke-command") view.unresolved = true; // Runs a script block UAM does not parse.
		if (name == "powershell" || name == "pwsh")
		{
			// -EncodedCommand and its abbreviations carry base64 code UAM cannot read.
			for (std::size_t j = i + 1; j < words.size(); ++j)
			{
				std::string option;
				for (char ch : words[j].text) option.push_back(Lower(ch));
				if (option.size() >= 2 && (option[0] == '-' || option[0] == '/') &&
				    (std::string_view("-encodedcommand").substr(1).starts_with(std::string_view(option).substr(1)) || option.substr(1) == "ec"))
					view.unresolved = true;
			}
		}
		if (name == "cmd")
		{
			// cmd accepts the command glued to its switch: `cmd /crm x`.
			for (std::size_t j = i + 1; j < words.size(); ++j)
			{
				const std::string_view word = words[j].text;
				if (word.size() > 2 && (word[0] == '/' || word[0] == '-') && (Lower(word[1]) == 'c' || Lower(word[1]) == 'k' || Lower(word[1]) == 'r'))
				{
					AddNested(view, rest_of_line(j + 1, std::string(word.substr(2))), depth);
					break;
				}
			}
		}
		// A program computed at run time might be a shell, so its -c or /c string is checked too.
		if (ShellProgram(name) || words[i].dynamic || !words[i].literal)
		{
			if (name == "eval" || name == "iex" || name == "invoke-expression") AddNested(view, rest_of_line(i + 2, i + 1 < words.size() ? words[i + 1].text : std::string{}), depth);
			for (std::size_t j = i + 1; j + 1 < words.size(); ++j)
				if (CommandStringOption(words[j].text))
				{
					// POSIX shells run the next word; cmd /c and PowerShell -Command run the rest of the line.
					AddNested(view, words[j + 1].text, depth);
					if (j + 2 < words.size()) AddNested(view, rest_of_line(j + 2, words[j + 1].text), depth);
					break;
				}
		}
		if (name == "git")
		{
			// Subcommands that run other commands: `submodule foreach <cmd>`, `bisect run <cmd>`, `rebase -x <cmd>`
			// and `difftool -x/--extcmd <cmd>`.
			const std::vector<std::size_t> git_operands = GitOperands(words, i + 1);
			const std::string sub = git_operands.empty() ? std::string{} : LowerText(words[git_operands[0]].text);
			for (std::size_t j = git_operands.empty() ? words.size() : git_operands[0] + 1; j < words.size(); ++j)
			{
				const std::string_view text = words[j].text;
				if ((sub == "submodule" && text == "foreach") || (sub == "bisect" && text == "run"))
				{
					std::size_t k = j + 1;
					while (k < words.size() && words[k].text.starts_with('-')) ++k;
					if (k < words.size()) AddNested(view, rest_of_line(k + 1, words[k].text), depth);
					break;
				}
				const bool runner = sub == "rebase" || sub == "difftool";
				if (runner && (text == "-x" || text == "--exec" || text == "--extcmd") && j + 1 < words.size()) AddNested(view, words[j + 1].text, depth);
				else if (runner && text.starts_with("--exec=")) AddNested(view, std::string(text.substr(7)), depth);
				else if (runner && text.starts_with("--extcmd=")) AddNested(view, std::string(text.substr(9)), depth);
			}
			// `git -c alias.x='push --force' x` defines and runs an alias on the command line; a `!` alias runs a shell.
			for (std::size_t j = i + 1; j + 1 < words.size(); ++j)
			{
				if (words[j].text != "-c") continue;
				std::string setting;
				for (char ch : words[j + 1].text) setting.push_back(Lower(ch));
				if (!setting.starts_with("alias.")) continue;
				const std::size_t equals = words[j + 1].text.find('=');
				if (equals == std::string::npos) continue;
				const std::string body = words[j + 1].text.substr(equals + 1);
				AddNested(view, body.starts_with('!') ? body.substr(1) : "git " + body, depth);
			}
		}
		if (name == "find" || name == "fd" || name == "parallel")
		{
			for (std::size_t j = i + 1; j + 1 < words.size(); ++j)
			{
				const std::string option = ProgramName(words[j].text);
				if (option == "-exec" || option == "-execdir" || option == "-ok" || option == "-okdir" || option == "--exec" || option == "-x" || option == "--exec-batch" || option == ":::")
					CollectFrom(segment_index, j + 1, view, depth, visited);
			}
		}
		if (!Wrapper(name)) return;
		if (name == "env")
		{
			// env -S / --split-string runs its value as a command line.
			for (std::size_t j = i + 1; j < words.size(); ++j)
			{
				const std::string_view word = words[j].text;
				if (word == "-S" || word == "--split-string") { if (j + 1 < words.size()) AddNested(view, words[j + 1].text, depth); }
				else if (word.starts_with("--split-string=")) AddNested(view, std::string(word.substr(15)), depth);
				else if (word.starts_with("-S")) AddNested(view, std::string(word.substr(2)), depth);
			}
		}
		for (std::size_t j = i + 1; j < words.size(); ++j) CollectFrom(segment_index, j, view, depth, visited);
	}

	inline void CollectInvocations(std::size_t segment_index, CommandView& view, int depth)
	{
		std::vector<char> visited(view.segments[segment_index].size(), 0);
		CollectFrom(segment_index, 0, view, depth, visited);
	}

	/// <summary>The command with quote characters and the cmd.exe escape removed and backslashes read as path
	/// separators. Lexing it as well exposes operators that only POSIX quoting hides (cmd.exe treats ' as text
	/// and has no backslash escape) and Windows paths, so deny and ask rules over-approximate across shells.</summary>
	inline std::string QuoteBlind(std::string_view command)
	{
		std::string blind;
		for (char ch : command)
		{
			if (ch == '\\') blind.push_back('/');
			else if (ch == '\v' || ch == '\f') blind.push_back(' '); // PowerShell separates words with these.
			else if (ch != '\'' && ch != '"' && ch != '^') blind.push_back(ch);
		}
		// A no-break space (UTF-8 C2 A0) also separates PowerShell words.
		for (std::size_t at = blind.find("\xC2\xA0"); at != std::string::npos; at = blind.find("\xC2\xA0", at)) blind.replace(at, 2, " ");
		return blind;
	}

	inline void AddSegments(Lexed& lexed, CommandView& view, int depth)
	{
		for (std::vector<Word>& segment : lexed.segments)
		{
			if (segment.size() > kMaxWordsPerSegment) { segment.resize(kMaxWordsPerSegment); view.unresolved = true; }
			if (view.total_words + segment.size() > kMaxTotalWords) { view.unresolved = true; break; }
			view.total_words += segment.size();
			// A brace list turns one word into several, which word-by-word patterns cannot follow.
			if (std::any_of(segment.begin(), segment.end(), [](const Word& word) { return word.brace_list; })) view.unresolved = true;
			view.segments.push_back(std::move(segment));
			CollectInvocations(view.segments.size() - 1, view, depth);
		}
		for (std::string& body : lexed.nested) AddNested(view, std::move(body), depth);
	}

	inline void Collect(std::string_view command, CommandView& view, int depth)
	{
		if (command.size() > kMaxCommandBytes) { view.unresolved = true; command = command.substr(0, kMaxCommandBytes); }
		Lexed lexed = Lex(command);
		if (!lexed.ok) view.unresolved = true;
		AddSegments(lexed, view, depth);
		if (const std::string blind = QuoteBlind(command); blind != command)
		{
			Lexed extra = Lex(blind);
			if (!extra.ok) view.unresolved = true;
			AddSegments(extra, view, depth);
		}
		std::vector<std::string> nested = std::move(view.nested);
		view.nested.clear();
		for (const std::string& body : nested) Collect(body, view, depth + 1);
	}

	/// <summary>One glob position: a run of any length (star) or one character, each from an allowed set.</summary>
	struct GlobToken
	{
		bool star = false;
		std::bitset<256> set;
	};

	inline void AddChar(std::bitset<256>& set, char ch, bool fold)
	{
		set.set(static_cast<unsigned char>(ch));
		if (fold && ch >= 'a' && ch <= 'z') set.set(static_cast<unsigned char>(ch - 'a' + 'A'));
		if (fold && ch >= 'A' && ch <= 'Z') set.set(static_cast<unsigned char>(ch - 'A' + 'a'));
	}

	/// <summary>Parses "[...]" starting at text[at]; returns the index after ']' or npos when not a class.</summary>
	inline std::size_t ParseClass(std::string_view text, std::size_t at, std::bitset<256>& set, bool fold)
	{
		const std::size_t close = text.find(']', at + 2);
		if (close == std::string_view::npos) return std::string_view::npos;
		std::size_t q = at + 1;
		const bool negate = text[q] == '!' || text[q] == '^';
		if (negate) ++q;
		std::bitset<256> chosen;
		for (; q < close; ++q)
		{
			if (q + 2 < close && text[q + 1] == '-')
			{
				for (int c = static_cast<unsigned char>(text[q]); c <= static_cast<unsigned char>(text[q + 2]); ++c) AddChar(chosen, static_cast<char>(c), fold);
				q += 2;
			}
			else AddChar(chosen, text[q], fold);
		}
		set = negate ? ~chosen : chosen;
		return close + 1;
	}

	/// <summary>A rule pattern word as glob tokens: '*' spans anything, including '/'.</summary>
	inline std::vector<GlobToken> PatternTokens(std::string_view pattern)
	{
		std::vector<GlobToken> tokens;
		for (std::size_t i = 0; i < pattern.size();)
		{
			GlobToken token;
			if (pattern[i] == '*') { token.star = true; token.set.set(); ++i; }
			else if (pattern[i] == '?') { token.set.set(); ++i; }
			else if (pattern[i] == '[')
			{
				const std::size_t next = ParseClass(pattern, i, token.set, true);
				if (next == std::string_view::npos) { AddChar(token.set, '[', true); ++i; }
				else i = next;
			}
			else { AddChar(token.set, pattern[i] == '\\' ? '/' : pattern[i], true); ++i; }
			tokens.push_back(token);
		}
		return tokens;
	}

	/// <summary>A command word the shell would expand, as glob tokens: unquoted '*' and '?' stop at '/', and a
	/// brace list, a variable or a substitution may become anything. Dot files are not excluded, because
	/// options such as bash's dotglob let '*' match them.</summary>
	inline std::vector<GlobToken> ExpansionTokens(const Word& word)
	{
		std::vector<GlobToken> tokens;
		std::bitset<256> no_slash;
		no_slash.set();
		no_slash.reset(static_cast<unsigned char>('/'));
		const std::string& text = word.text;
		for (std::size_t i = 0; i < text.size();)
		{
			const unsigned char meta = i < word.meta.size() ? word.meta[i] : 0;
			GlobToken token;
			if (meta == kDynamicMark)
			{
				token.star = true;
				token.set.set();
				while (i < text.size() && word.meta[i] == kDynamicMark) ++i;
			}
			else if (meta == kGlobMark && text[i] == '*') { token.star = true; token.set = no_slash; ++i; }
			else if (meta == kGlobMark && text[i] == '?') { token.set = no_slash; ++i; }
			else if (meta == kGlobMark && text[i] == '{' && word.brace_list)
			{
				token.star = true;
				token.set.set();
				int depth = 0;
				for (; i < text.size(); ++i)
				{
					if (word.meta[i] == kGlobMark && text[i] == '{') ++depth;
					else if (word.meta[i] == kGlobMark && text[i] == '}' && --depth == 0) { ++i; break; }
				}
			}
			else if (meta == kGlobMark && text[i] == '[')
			{
				const std::size_t next = ParseClass(text, i, token.set, true);
				if (next == std::string::npos) { AddChar(token.set, '[', true); ++i; }
				else i = next;
			}
			else { AddChar(token.set, text[i] == '\\' ? '/' : text[i], true); ++i; }
			tokens.push_back(token);
		}
		return tokens;
	}

	/// <summary>Whether two globs can match a common string (both sides case-folded).</summary>
	inline bool GlobsIntersect(const std::vector<GlobToken>& a, const std::vector<GlobToken>& b)
	{
		const std::size_t width = b.size() + 1;
		std::vector<char> reach((a.size() + 1) * width, 0);
		reach[0] = 1;
		// reach[i][j]: some string takes a's first i tokens and b's first j tokens to the same point.
		for (std::size_t i = 0; i <= a.size(); ++i)
			for (std::size_t j = 0; j <= b.size(); ++j)
			{
				if (!reach[i * width + j]) continue;
				const bool a_star = i < a.size() && a[i].star;
				const bool b_star = j < b.size() && b[j].star;
				if (a_star) reach[(i + 1) * width + j] = 1; // a's run ends here.
				if (b_star) reach[i * width + j + 1] = 1;
				// Consume one shared character: a run stays open, a single position advances.
				if (i < a.size() && j < b.size() && (a[i].set & b[j].set).any())
					reach[(a_star ? i : i + 1) * width + (b_star ? j : j + 1)] = 1;
			}
		return reach[a.size() * width + b.size()] != 0;
	}

	/// <summary>How surely a pattern word matches a command word: a literal word matches definitely or not at
	/// all; a word the shell expands (glob, brace list, variable, substitution) only possibly, when what it could
	/// become overlaps the pattern. A possible match holds the request for the user; only a definite one denies.</summary>
	enum Certainty : char
	{
		kNoMatch = 0,
		kPossibleMatch = 1,
		kDefiniteMatch = 2,
	};

	/// <summary>Compares one rule pattern word with one command word.</summary>
	inline Certainty WordMatches(const Word& part, const Word& word, bool program_position, bool deny_side)
	{
		if (!deny_side) return GlobMatch(part.text, word.text) ? kDefiniteMatch : kNoMatch;
		// Program words that expand are already unresolved; compare them as written.
		if (program_position && part.text.find_first_of("/\\") == std::string::npos)
			return GlobMatch(ProgramName(part.text), ProgramName(word.text), true) ? kDefiniteMatch : kNoMatch;
		if (word.literal && !word.dynamic)
		{
			std::string normalized = word.text;
			std::replace(normalized.begin(), normalized.end(), '\\', '/');
			return GlobMatch(part.text, normalized, true) ? kDefiniteMatch : kNoMatch;
		}
		if (word.text.size() > kMaxGlobWordBytes || part.text.size() > kMaxGlobWordBytes) return kPossibleMatch; // Too large to compare.
		return GlobsIntersect(PatternTokens(part.text), ExpansionTokens(word)) ? kPossibleMatch : kNoMatch;
	}

	inline bool IsSpan(const Word& part) { return part.text == "**" && !part.quoted; }

	/// <summary>How surely the pattern matches the start of words[start..). Linear in pattern x words: `**` spans
	/// any number of words through a running maximum instead of backtracking; a path's certainty is the weakest
	/// word comparison along it.</summary>
	inline Certainty MatchFrom(const std::vector<Word>& pattern, const std::vector<Word>& words, std::size_t start, bool deny_side,
	    const std::vector<char>* argument_table = nullptr)
	{
		const std::size_t count = words.size() - std::min(start, words.size());
		std::vector<char> current(count + 1, 0), next(count + 1, 0);
		current[0] = kDefiniteMatch;
		for (std::size_t p = 0; p < pattern.size(); ++p)
		{
			std::fill(next.begin(), next.end(), 0);
			if (IsSpan(pattern[p]))
			{
				char running = 0;
				for (std::size_t w = 0; w <= count; ++w) next[w] = running = std::max(running, current[w]);
			}
			else
			{
				for (std::size_t w = 0; w < count; ++w)
				{
					if (!current[w]) continue;
					const char matched = argument_table != nullptr && w > 0
					    ? (*argument_table)[p * words.size() + start + w]
					    : static_cast<char>(WordMatches(pattern[p], words[start + w], w == 0, deny_side));
					if (matched) next[w + 1] = std::max(next[w + 1], std::min(current[w], matched));
				}
			}
			std::swap(current, next);
			if (std::none_of(current.begin(), current.end(), [](char value) { return value != 0; })) return kNoMatch;
		}
		return static_cast<Certainty>(*std::max_element(current.begin(), current.end()));
	}

	inline std::vector<Word> PatternWords(std::string_view pattern)
	{
		Lexed lexed = LexPattern(pattern);
		std::vector<Word> words;
		for (std::vector<Word>& segment : lexed.segments)
			for (Word& word : segment) words.push_back(std::move(word));
		return words;
	}

	/// <summary>Deny/ask side: how surely any program invocation anywhere in the command, including wrapped,
	/// chained, substituted and shell -c commands, starts with the pattern. Argument comparisons are computed
	/// once per segment and shared by every invocation inside it.</summary>
	inline Certainty DenyPatternMatches(const std::vector<Word>& pattern, const CommandView& view)
	{
		if (pattern.empty()) return kDefiniteMatch;
		Certainty best = kNoMatch;
		std::vector<std::vector<char>> tables(view.segments.size());
		for (const Invocation& invocation : view.invocations)
		{
			const std::vector<Word>& words = view.segments[invocation.segment];
			std::vector<char>& table = tables[invocation.segment];
			if (table.empty())
			{
				table.assign(pattern.size() * words.size(), 0);
				for (std::size_t p = 0; p < pattern.size(); ++p)
					if (!IsSpan(pattern[p]))
						for (std::size_t w = 0; w < words.size(); ++w) table[p * words.size() + w] = static_cast<char>(WordMatches(pattern[p], words[w], false, true));
			}
			best = std::max(best, MatchFrom(pattern, words, invocation.start, true, &table));
			if (best == kDefiniteMatch) break;
		}
		return best;
	}


	/// <summary>Whether the program starting at words[start] commits permanently to a remote: git push and the
	/// Git commands that push (send-pack, svn dcommit, p4 submit, subtree and lfs push, git-* programs, hub,
	/// possible config aliases), gh pr merge, pr close --delete-branch, repo sync, repo create --push, release
	/// create/delete and mutating gh api calls, and SVN commits, imports, URL operations, revision and URL
	/// property changes, svnmucc, svnrdump load and svnsync. Words the shell expands make it a possible match,
	/// and so does a subcommand UAM does not recognise.</summary>
	inline Certainty RemoteCommitAt(const std::vector<Word>& words, std::size_t start)
	{
		std::string name = ProgramName(words[start].text);
		const auto expanded = [](const Word& word) { return word.dynamic || !word.literal; };
		const auto any_word = [&](std::string_view text) { return std::any_of(words.begin() + static_cast<std::ptrdiff_t>(start + 1), words.end(), [&](const Word& word) { return LowerText(word.text) == text; }); };
		const auto remote_target = [](const Word& word) { return word.text.find("://") != std::string::npos || word.text.starts_with("^/"); };
		// Arguments appended by xargs or parallel are invisible here.
		const bool fed = std::any_of(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(start), [](const Word& word) { const std::string wrapper = ProgramName(word.text); return wrapper == "xargs" || wrapper == "parallel"; });

		if (name == "hub")
		{
			// hub is git with GitHub commands: `hub pr merge` merges, everything else behaves like git.
			const std::vector<std::size_t> operands = GitOperands(words, start + 1);
			if (operands.size() > 1 && LowerText(words[operands[0]].text) == "pr" && any_word("merge")) return kDefiniteMatch;
			name = "git";
		}
		std::string git_command;
		std::size_t git_next = 0; // Index into operands of the word after the subcommand.
		std::vector<std::size_t> operands;
		if (name == "git")
		{
			operands = GitOperands(words, start + 1);
			if (operands.empty()) return fed ? kPossibleMatch : kNoMatch;
			const Word& sub = words[operands[0]];
			if (expanded(sub)) return kPossibleMatch;
			git_command = LowerText(sub.text);
			git_next = 1;
		}
		else if (name.starts_with("git-") && name.size() > 4)
		{
			// Git's own programs: git-push, git-send-pack, git-svn, git-subtree, git-lfs and the rest.
			operands = GitOperands(words, start + 1);
			git_command = name.substr(4);
			git_next = 0;
		}
		if (!git_command.empty())
		{
			const std::string next = operands.size() > git_next ? LowerText(words[operands[git_next]].text) : std::string{};
			if (OneOf(git_command, {"push", "send-pack", "http-push"})) return kDefiniteMatch;
			if (git_command == "svn") return OneOf(next, {"dcommit", "commit-diff", "set-tree", "branch", "tag"}) ? kDefiniteMatch : kNoMatch;
			if (git_command == "p4") return next == "submit" ? kDefiniteMatch : kNoMatch;
			if (OneOf(git_command, {"subtree", "lfs"})) return next == "push" ? kDefiniteMatch : kNoMatch;
			if (git_command == "annex") return OneOf(next, {"sync", "copy", "move", "export"}) ? kPossibleMatch : kNoMatch;
			return LocalGitSubcommand(git_command) ? kNoMatch : kPossibleMatch;
		}
		if (name == "gh")
		{
			operands = Operands(words, start + 1, {"-R", "--repo", "--hostname", "-X", "--method", "-f", "-F", "--field", "--raw-field",
			    "--input", "-H", "--header", "-q", "--jq", "-t", "--template", "--cache", "-p", "--preview"});
			if (operands.empty()) return fed ? kPossibleMatch : kNoMatch;
			if (std::any_of(operands.begin(), operands.begin() + std::min<std::ptrdiff_t>(2, static_cast<std::ptrdiff_t>(operands.size())), [&](std::size_t at) { return expanded(words[at]); })) return kPossibleMatch;
			// gh skips a flag it does not know together with its value while finding the command, so a value can
			// sit before the command words: look for them in order anywhere among the operands.
			const auto then = [&](std::string_view first, std::initializer_list<std::string_view> second)
			{
				for (std::size_t a = 0; a < operands.size(); ++a)
					if (LowerText(words[operands[a]].text) == first)
						for (std::size_t b = a + 1; b < operands.size(); ++b)
							if (OneOf(LowerText(words[operands[b]].text), second)) return true;
				return false;
			};
			if (then("pr", {"merge"}) || then("release", {"create", "delete"}) || then("repo", {"sync"})) return kDefiniteMatch;
			if (then("pr", {"close"}) && any_word("--delete-branch")) return kDefiniteMatch;
			if (then("repo", {"create"}) && any_word("--push")) return kDefiniteMatch;
			const std::string group = LowerText(words[operands[0]].text);
			// A command gh does not ship may be an alias or an extension.
			if (!OneOf(group, {"alias", "api", "attestation", "auth", "browse", "cache", "co", "codespace", "completion", "config", "gist", "gpg-key", "help",
			        "issue", "label", "org", "pr", "project", "release", "repo", "ruleset", "run", "search", "secret", "ssh-key", "status", "variable",
			        "workflow", "version"}))
				return kPossibleMatch;
			if (group != "api") return kNoMatch;
			// gh api is a GET unless a method says otherwise, or fields or an input body make it a POST.
			Certainty result = kNoMatch;
			bool explicit_get = false, body = false, mutating_method = false;
			for (std::size_t i = start + 1; i < words.size(); ++i)
			{
				const std::string text = words[i].text;
				std::string method;
				if ((text == "-X" || text == "--method") && i + 1 < words.size()) { if (expanded(words[i + 1])) result = kPossibleMatch; method = words[i + 1].text; }
				else if (text.starts_with("--method=")) method = text.substr(9);
				else if (text.starts_with("-X") && text.size() > 2) method = text.substr(2);
				if (!method.empty()) { if (LowerText(method) == "get") explicit_get = true; else mutating_method = true; }
				if (OneOf(text, {"-f", "-F", "--field", "--raw-field", "--input"}) || text.starts_with("--field=") || text.starts_with("--raw-field=") ||
				    text.starts_with("--input=") || (text.starts_with("-f") && text.size() > 2) || (text.starts_with("-F") && text.size() > 2)) body = true;
			}
			if (mutating_method || (body && !explicit_get)) return kDefiniteMatch;
			return result;
		}
		if (name == "svnmucc") return Operands(words, start + 1, {"-m", "--message", "-F", "--file", "-U", "--root-url", "-r", "--revision", "-u", "--username", "-p", "--password", "--config-dir", "--config-option", "-X", "--extra-args"}).empty() ? kNoMatch : kDefiniteMatch;
		if (name == "svnrdump") return any_word("load") ? kDefiniteMatch : kNoMatch;
		if (name == "svnsync") return any_word("init") || any_word("initialize") || any_word("sync") || any_word("synchronize") || any_word("copy-revprops") ? kDefiniteMatch : kNoMatch;
		if (name == "svn")
		{
			operands = Operands(words, start + 1, {"-m", "--message", "-F", "--file", "--username", "--password", "--config-dir",
			    "--config-option", "-r", "--revision", "--depth", "--set-depth", "--targets", "--encoding", "--with-revprop", "--changelist", "--cl", "--accept",
			    "--native-eol", "-c", "--change", "--editor-cmd", "--diff-cmd", "--diff3-cmd", "-l", "--limit", "-x", "--extensions",
			    "--trust-server-cert-failures", "--show-item", "--search", "--search-and", "--strip", "--old", "--new", "--password-from-stdin"});
			if (operands.empty()) return fed ? kPossibleMatch : kNoMatch;
			const Word& sub = words[operands[0]];
			if (expanded(sub)) return kPossibleMatch;
			const std::string command = LowerText(sub.text);
			if (OneOf(command, {"commit", "ci", "import"})) return kDefiniteMatch;
			const auto targets = [&](std::size_t from) -> Certainty
			{
				Certainty result = kNoMatch;
				for (std::size_t k = from; k < operands.size(); ++k)
				{
					const Word& operand = words[operands[k]];
					if (remote_target(operand)) return kDefiniteMatch;
					if (expanded(operand)) result = kPossibleMatch;
				}
				return result;
			};
			// Copy and move commit only when the destination (the last operand) is in the repository.
			if (OneOf(command, {"copy", "cp", "move", "mv", "rename", "ren"})) return operands.size() > 1 ? targets(operands.size() - 1) : kNoMatch;
			if (OneOf(command, {"mkdir", "delete", "del", "remove", "rm", "lock", "unlock"}))
				return OneOf(command, {"lock", "unlock"}) ? kNoMatch : targets(1);
			if (OneOf(command, {"propset", "pset", "ps", "propdel", "pdel", "pd", "propedit", "pedit", "pe"}))
				return any_word("--revprop") ? kDefiniteMatch : targets(1);
			// A subcommand svn does not ship (or an option value UAM cannot place) cannot be ruled out.
			return OneOf(command, {"add", "auth", "blame", "praise", "annotate", "ann", "cat", "changelist", "cl", "checkout", "co", "cleanup", "diff", "di",
			           "export", "help", "h", "?", "info", "list", "ls", "log", "merge", "mergeinfo", "patch", "propget", "pget", "pg", "proplist", "plist", "pl",
			           "relocate", "resolve", "resolved", "revert", "status", "stat", "st", "switch", "sw", "update", "up", "upgrade"}) ? kNoMatch : kPossibleMatch;
		}
		return kNoMatch;
	}

	inline Certainty RemoteCommitMatches(const CommandView& view)
	{
		Certainty best = kNoMatch;
		for (const Invocation& invocation : view.invocations)
		{
			best = std::max(best, RemoteCommitAt(view.segments[invocation.segment], invocation.start));
			if (best == kDefiniteMatch) break;
		}
		return best;
	}

	/// <summary>Allow side: the command must be one plain simple command with no operators, redirection,
	/// substitution, expansion, glob or unclosed quote; the pattern must match from its very first word (no
	/// leading assignments or wrappers) and the program must be a bare name. Anything else falls through to
	/// the normal flow.</summary>
	inline bool AllowPatternMatches(const std::vector<Word>& pattern, std::string_view command)
	{
		if (pattern.empty() || command.empty() || command.size() > kMaxCommandBytes) return false;
		// Providers may run commands through cmd.exe or PowerShell, where single quotes, '#' and backslashes do not
		// hide operators and vertical tabs, form feeds and no-break spaces separate words. An allowed command
		// therefore contains none of them, quoted or not.
		if (command.find_first_of(";&|<>()$`%!\n\r\v\f") != std::string_view::npos || command.find("\xC2\xA0") != std::string_view::npos) return false;
		const Lexed lexed = Lex(command);
		if (!lexed.ok || lexed.compound || lexed.dynamic || lexed.segments.size() != 1) return false;
		const std::vector<Word>& words = lexed.segments.front();
		if (words.empty() || words.size() > kMaxWordsPerSegment || Assignment(words.front()) || words.front().text.find_first_of("/\\") != std::string::npos) return false;
		// Every word must be literal and static, so no expansion (including a home-directory '~') can change what the
		// matched words say.
		if (std::any_of(words.begin(), words.end(), [](const Word& word) { return word.dynamic || !word.literal || word.text.starts_with('~'); })) return false;
		// The first word compares exactly (a case-sensitive bare name) on the allow side.
		return MatchFrom(pattern, words, 0, false) == kDefiniteMatch;
	}

	inline bool SamePathOrInside(std::string_view base, std::string_view path)
	{
		const auto normalize = [](std::string_view value)
		{
			std::string out;
			for (char ch : value)
			{
				char c = ch == '\\' ? '/' : ch;
#if defined(_WIN32) || defined(__APPLE__)
				c = Lower(c);
#endif
				if (c == '/' && !out.empty() && out.back() == '/') continue;
				out.push_back(c);
			}
			while (out.size() > 1 && out.back() == '/') out.pop_back();
			return out;
		};
		const std::string b = normalize(base);
		const std::string p = normalize(path);
		if (b.empty() || p.empty()) return false;
		if (p == b) return true;
		return p.size() > b.size() && p.compare(0, b.size(), b) == 0 && (b.back() == '/' || p[b.size()] == '/');
	}

	inline bool ScopeMatches(const Rule& rule, const Context& context)
	{
		if (!rule.enabled) return false;
		if (context.ignore_scope) return true;
		if (!rule.provider_id.empty() && rule.provider_id != context.provider_id) return false;
		if (!rule.workspace.empty() && !SamePathOrInside(rule.workspace, context.workspace) &&
		    !SamePathOrInside(rule.workspace, context.source_workspace) && !SamePathOrInside(rule.workspace, context.folder_workspace)) return false;
		return true;
	}

	inline bool ToolMatches(Tool tool, const Context& context)
	{
		switch (tool)
		{
		case Tool::Any: return true;
		case Tool::Command: return context.category == Category::Command;
		case Tool::ReadOnly: return context.read_only && (context.category == Category::Command || context.category == Category::Read);
		case Tool::Edit: return context.category == Category::Edit;
		case Tool::Fetch: return context.category == Category::Fetch;
		case Tool::Other: return context.category == Category::Other;
		}
		return false;
	}
} // namespace detail

/// <summary>Why a rule is invalid, or empty when it can be saved. Allow rules must be specific: they cannot
/// target every tool, web access or unclassified tools, and an allow command rule needs a pattern.</summary>
inline std::string ValidationError(const Rule& rule)
{
	if (rule.id.empty() || rule.id.size() > 128) return "Each rule needs an id of at most 128 characters.";
	if (rule.pattern.size() > kMaxPatternBytes) return "A command pattern can be at most 512 characters.";
	if (rule.provider_id.size() > 128 || rule.workspace.size() > kMaxFieldBytes) return "The provider or workspace is too long.";
	for (std::string_view field : {std::string_view(rule.pattern), std::string_view(rule.provider_id), std::string_view(rule.workspace)})
		if (std::any_of(field.begin(), field.end(), [](char ch) { return ch == '\n' || ch == '\r' || ch == '\0'; })) return "Rules must be a single line.";
	if (rule.tool != Tool::Command && !rule.pattern.empty()) return "Only command rules take a pattern.";
	if (!rule.workspace.empty())
	{
		const std::string_view folder = rule.workspace;
		const bool absolute = folder.starts_with('/') || folder.starts_with("\\\\") ||
		    (folder.size() >= 3 && ((folder[0] >= 'A' && folder[0] <= 'Z') || (folder[0] >= 'a' && folder[0] <= 'z')) && folder[1] == ':' && (folder[2] == '/' || folder[2] == '\\'));
		if (!absolute) return "A workspace must be a full folder path.";
	}
	if (rule.tool == Tool::Command)
	{
		if (rule.pattern.find_first_not_of(" \t") == std::string::npos) return "A command rule needs a pattern.";
		const detail::Lexed lexed = detail::LexPattern(rule.pattern);
		if (!lexed.ok) return "Close every quote in the pattern.";
		if (lexed.compound || lexed.segments.size() != 1) return "A pattern is one command: no ;, &&, |, redirection or $( ).";
	}
	if (rule.action == Action::Allow && (rule.tool == Tool::Any || rule.tool == Tool::Fetch || rule.tool == Tool::Other))
		return "Allow rules can target commands, read-only work or file edits only.";
	if (rule.action == Action::Allow && rule.tool == Tool::Command)
	{
		const std::vector<detail::Word> words = detail::PatternWords(rule.pattern);
		if (words.empty() || words.front().text == "**" || words.front().text.find_first_of("*?[/\\") != std::string::npos)
			return "An allow pattern must start with a program name.";
	}
	return {};
}

/// <summary>Drops invalid and duplicate rules and caps the list, keeping order.</summary>
inline std::vector<Rule> Normalize(std::vector<Rule> rules)
{
	std::vector<Rule> result;
	for (Rule& rule : rules)
	{
		if (result.size() >= kMaxRules) break;
		if (!ValidationError(rule).empty()) continue;
		if (std::any_of(result.begin(), result.end(), [&](const Rule& existing) { return existing.id == rule.id; })) continue;
		result.push_back(std::move(rule));
	}
	return result;
}

/// <summary>Seeded on first load: read-only work skips AI review; deletion always asks; force pushes and
/// credential stores are denied.</summary>
inline std::vector<Rule> DefaultRules()
{
	const auto rule = [](std::string id, Action action, Tool tool, std::string pattern = {})
	{
		Rule value;
		value.id = std::move(id);
		value.action = action;
		value.tool = tool;
		value.pattern = std::move(pattern);
		return value;
	};
	return {
	    rule("default-read-only", Action::Allow, Tool::ReadOnly),
	    rule("default-rm", Action::Ask, Tool::Command, "rm"),
	    rule("default-rmdir", Action::Ask, Tool::Command, "rmdir"),
	    rule("default-unlink", Action::Ask, Tool::Command, "unlink"),
	    rule("default-shred", Action::Ask, Tool::Command, "shred"),
	    rule("default-find-delete", Action::Ask, Tool::Command, "find ** -delete"),
	    rule("default-del", Action::Ask, Tool::Command, "del"),
	    rule("default-erase", Action::Ask, Tool::Command, "erase"),
	    rule("default-rd", Action::Ask, Tool::Command, "rd"),
	    rule("default-remove-item", Action::Ask, Tool::Command, "remove-item"),
	    rule("default-ri", Action::Ask, Tool::Command, "ri"),
	    rule("default-env-files", Action::Ask, Tool::Command, "** *.env"),
	    rule("default-env-local", Action::Ask, Tool::Command, "** *.env.local"),
	    rule("default-env-production", Action::Ask, Tool::Command, "** *.env.production"),
	    rule("default-env-development", Action::Ask, Tool::Command, "** *.env.development"),
	    rule("default-force-push", Action::Deny, Tool::Command, "git ** push ** --force*"),
	    rule("default-force-push-short", Action::Deny, Tool::Command, "git ** push ** -f"),
	    rule("default-force-push-combined", Action::Deny, Tool::Command, "git ** push ** -[!-]*f*"),
	    rule("default-force-push-refspec", Action::Deny, Tool::Command, "git ** push ** +*"),
	    rule("default-ssh-keys", Action::Deny, Tool::Command, "** *.ssh/*"),
	    rule("default-aws-credentials", Action::Deny, Tool::Command, "** *.aws/credentials*"),
	    rule("default-keychain-passwords", Action::Deny, Tool::Command, "security find-*-password"),
	    rule("default-keychain-dump", Action::Deny, Tool::Command, "security dump-keychain"),
	    rule("default-git-credentials", Action::Deny, Tool::Command, "git ** credential*"),
	    rule("default-gh-token", Action::Deny, Tool::Command, "gh ** auth ** token"),
	};
}

/// <summary>How surely the rule matches the request (ignoring whether an allow would be permitted). Allow and
/// non-command rules match definitely or not at all.</summary>
inline detail::Certainty RuleMatches(const Rule& rule, const Context& context, const detail::CommandView* view)
{
	if (!detail::ScopeMatches(rule, context) || !detail::ToolMatches(rule.tool, context)) return detail::kNoMatch;
	if (rule.tool != Tool::Command) return detail::kDefiniteMatch;
	const std::vector<detail::Word> pattern = detail::PatternWords(rule.pattern);
	if (rule.action == Action::Allow) return detail::AllowPatternMatches(pattern, context.command) ? detail::kDefiniteMatch : detail::kNoMatch;
	return view != nullptr ? detail::DenyPatternMatches(pattern, *view) : detail::kNoMatch;
}

/// <summary>Evaluates every enabled rule. Deny wins over ask, ask over allow. A deny or ask rule the command
/// might match once the shell expands it, and a command whose program is computed at run time (or that does not
/// parse), cannot be proven clear of those rules, so it is left for the user whenever such rules exist. An allow
/// match approves only when the caller's ceiling permits; otherwise the normal flow continues.</summary>
inline Decision Evaluate(const std::vector<Rule>& rules, const Context& context)
{
	Decision decision;
	detail::CommandView view;
	const bool command = context.category == Category::Command;
	// A request with no typed command gives command rules nothing to read; only any-request rules apply.
	if (command) detail::Collect(context.command, view, 0);

	const Rule* deny = nullptr;
	const Rule* ask = nullptr;
	const Rule* possible = nullptr;
	const Rule* allow = nullptr;
	// Built-in, non-removable ask rule: remote commits wait for the user unless the hidden override is set.
	const bool remote_rule = command && !context.remote_commit_override;
	const detail::Certainty remote = remote_rule ? detail::RemoteCommitMatches(view) : detail::kNoMatch;
	bool command_guard = remote_rule;
	bool user_command_guard = false;
	for (const Rule& rule : rules)
	{
		if (!rule.enabled) continue;
		if (rule.action != Action::Allow && rule.tool == Tool::Command && detail::ScopeMatches(rule, context)) command_guard = user_command_guard = true;
		const detail::Certainty match = RuleMatches(rule, context, command ? &view : nullptr);
		if (match == detail::kNoMatch) continue;
		if (match == detail::kPossibleMatch) { if (possible == nullptr) possible = &rule; continue; }
		if (rule.action == Action::Deny && deny == nullptr) deny = &rule;
		else if (rule.action == Action::Ask && ask == nullptr) ask = &rule;
		else if (rule.action == Action::Allow && allow == nullptr) allow = &rule;
	}
	if (deny != nullptr)
	{
		decision.outcome = Outcome::Deny;
		decision.rule_id = deny->id;
		decision.reason = "A deny rule matched.";
		return decision;
	}
	if (remote == detail::kDefiniteMatch || (remote == detail::kPossibleMatch && ask == nullptr))
	{
		decision.outcome = Outcome::AskUser;
		decision.rule_id = std::string(kRemoteCommitRuleId);
		decision.reason = std::string(kRemoteCommitNotice);
		decision.remote_commit = true;
		return decision;
	}
	if (ask != nullptr)
	{
		decision.outcome = Outcome::AskUser;
		decision.rule_id = ask->id;
		decision.reason = "An ask rule matched; the request waits for you.";
		return decision;
	}
	if (possible != nullptr)
	{
		decision.outcome = Outcome::AskUser;
		decision.rule_id = possible->id;
		decision.reason = "Once the shell expands it, this command might match a rule, so it waits for you.";
		return decision;
	}
	if (command && !context.command.empty() && view.unresolved && command_guard)
	{
		decision.outcome = Outcome::AskUser;
		if (user_command_guard) decision.reason = "UAM cannot tell exactly what this command runs, so deny and ask rules cannot be checked.";
		else
		{
			// Only the built-in rule applies: say so, and show it on the approval card like a held push.
			decision.rule_id = std::string(kRemoteCommitRuleId);
			decision.reason = "UAM cannot tell what this command runs, so it cannot rule out a push or other remote commit; it waits for your approval.";
			decision.remote_commit = true;
		}
		return decision;
	}
	if (allow != nullptr)
	{
		if (context.allow_permitted)
		{
			decision.outcome = Outcome::Approve;
			decision.rule_id = allow->id;
			decision.reason = "An allow rule matched.";
		}
		else
		{
			decision.allow_blocked_by_ceiling = true;
			decision.rule_id = allow->id;
		}
	}
	return decision;
}

/// <summary>Names that usually hold secrets. Read-only work naming one never skips review through a rule; a
/// substring check over-approximates, which only sends more requests to the reviewer or the user.</summary>
inline bool LooksCredentialLike(std::string_view text)
{
	std::string lower;
	for (char ch : text) lower.push_back(detail::Lower(ch == '\\' ? '/' : ch));
	constexpr std::string_view markers[] = {
	    ".env", "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519", ".pem", ".key", ".p12", ".pfx", ".keystore", ".jks", ".netrc", ".npmrc", ".pypirc",
	    ".git-credentials", "credential", ".ssh", ".aws", ".gnupg", ".kube", ".docker/config", "secret", "token", "password", "keychain"};
	return std::any_of(std::begin(markers), std::end(markers), [&](std::string_view marker) { return lower.find(marker) != std::string::npos; });
}

/// <summary>For the settings test field: which rule decides a typed command, as if it were a shell command
/// request with every allow permitted and every rule's provider and workspace applying; the caller reports a
/// deciding rule's scope. The real ceiling can still refuse an allow at run time.</summary>
inline Decision TestCommand(const std::vector<Rule>& rules, std::string_view command, bool read_only, bool remote_commit_override = false)
{
	Context context;
	context.category = Category::Command;
	context.command = command;
	context.read_only = read_only && !LooksCredentialLike(command);
	context.ignore_scope = true;
	context.remote_commit_override = remote_commit_override;
	context.allow_permitted = true;
	return Evaluate(rules, context);
}

} // namespace uam::command_rules
