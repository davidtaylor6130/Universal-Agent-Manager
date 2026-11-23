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
};

inline constexpr std::size_t kMaxRules = 200;
inline constexpr std::size_t kMaxPatternBytes = 512;
inline constexpr std::size_t kMaxFieldBytes = 1024;
inline constexpr std::size_t kMaxCommandBytes = 64 * 1024;
inline constexpr int kMaxNesting = 4;

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
	if (uses_network || lower == "fetch") return Category::Fetch;
	if (lower == "commandexecution" || lower == "execute") return Category::Command;
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
	};

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
		const auto end_word = [&]
		{
			if (!in_word) return;
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
				word.text.push_back(text[i + 1]);
				in_word = true;
				i += 2;
				continue;
			}
			if (ch == '\'')
			{
				const std::size_t close = text.find('\'', i + 1);
				if (close == std::string_view::npos) { out.ok = false; word.text.append(text.substr(i + 1)); in_word = true; break; }
				word.text.append(text.substr(i + 1, close - i - 1));
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
						if (next == '"' || next == '\\' || next == '$' || next == '`') word.text.push_back(next);
						else if (next != '\n') { word.text.push_back('\\'); word.text.push_back(next); }
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
						word.dynamic = out.dynamic = true;
					}
					word.text.push_back(inner);
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
				word.dynamic = out.dynamic = true;
				word.text.push_back(ch);
				in_word = true;
				++i;
				continue;
			}
			if (ch == '#' && !in_word)
			{
				while (i < text.size() && text[i] != '\n') ++i;
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
			if (ch == '*' || ch == '?' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || (ch == '~' && !in_word))
				word.literal = false;
			word.text.push_back(ch);
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
		if (name.size() > 4 && name.ends_with(".exe")) name.resize(name.size() - 4);
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
		    "!", "{", "}", "start", "start-process", "runas", "flock", "chroot", "systemd-run", "script", "busybox", "nsenter", "unshare", "firejail"};
		return std::find(std::begin(wrappers), std::end(wrappers), name) != std::end(wrappers);
	}

	/// <summary>Wrapper options whose value is the next word.</summary>
	inline bool WrapperOptionTakesValue(std::string_view wrapper, std::string_view option)
	{
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
		constexpr std::string_view shells[] = {"bash", "sh", "zsh", "dash", "ksh", "mksh", "csh", "tcsh", "fish", "ash", "busybox", "cmd", "powershell", "pwsh", "wsl", "su", "eval", "script"};
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

	struct CommandView
	{
		std::vector<std::vector<Word>> invocations; // Each starts at a program word.
		std::vector<std::string> nested;            // Command strings run by shells, eval and substitutions.
		bool unresolved = false;                    // A program word is computed at run time or the text did not lex.
	};

	/// <summary>Every place a program can start inside one simple command, following wrappers such as sudo,
	/// env, xargs and timeout, find -exec, and shell -c strings. Over-approximates on purpose (deny side).</summary>
	inline void CollectInvocations(const std::vector<Word>& words, CommandView& view, int depth)
	{
		std::size_t i = 0;
		while (i < words.size() && Assignment(words[i])) ++i;
		while (i < words.size())
		{
			if (words[i].dynamic) view.unresolved = true;
			view.invocations.emplace_back(words.begin() + static_cast<std::ptrdiff_t>(i), words.end());
			const std::string name = ProgramName(words[i].text);
			if (ShellProgram(name) || name == "eval")
			{
				if (name == "eval")
				{
					std::string joined;
					for (std::size_t j = i + 1; j < words.size(); ++j) joined += (j > i + 1 ? " " : "") + words[j].text;
					if (depth < kMaxNesting) view.nested.push_back(std::move(joined));
					else view.unresolved = true;
				}
				for (std::size_t j = i + 1; j + 1 < words.size(); ++j)
					if (CommandStringOption(words[j].text))
					{
						if (depth < kMaxNesting) view.nested.push_back(words[j + 1].text);
						else view.unresolved = true;
					}
			}
			if (name == "find" || name == "fd" || name == "git" || name == "parallel")
			{
				for (std::size_t j = i + 1; j + 1 < words.size(); ++j)
				{
					const std::string option = ProgramName(words[j].text);
					if (option == "-exec" || option == "-execdir" || option == "-ok" || option == "-okdir" || option == "--exec" || option == "-x" || option == "--exec-batch" || option == "-X" || option == ":::")
						view.invocations.emplace_back(words.begin() + static_cast<std::ptrdiff_t>(j + 1), words.end());
				}
			}
			if (!Wrapper(name)) break;
			// Skip the wrapper's own options, numbers, durations and assignments to reach the program it runs.
			const std::string wrapper = name;
			for (++i; i < words.size(); ++i)
			{
				const std::string_view word = words[i].text;
				if (Assignment(words[i])) continue;
				if (word.starts_with('-') && word.size() > 1)
				{
					// env -S / --split-string runs its value as a command line.
					if (wrapper == "env" && (word == "-S" || word.starts_with("--split-string")))
					{
						const std::size_t equals = word.find('=');
						if (equals != std::string_view::npos) view.nested.emplace_back(word.substr(equals + 1));
						else if (i + 1 < words.size()) view.nested.push_back(words[++i].text);
						continue;
					}
					if (wrapper == "env" && word.starts_with("-S")) { view.nested.emplace_back(word.substr(2)); continue; }
					if (WrapperOptionTakesValue(wrapper, word) && word.find('=') == std::string_view::npos) ++i;
					continue;
				}
				if (!word.empty() && word.front() >= '0' && word.front() <= '9') continue;
				break;
			}
		}
	}

	inline void Collect(std::string_view command, CommandView& view, int depth)
	{
		Lexed lexed = Lex(command.substr(0, std::min(command.size(), kMaxCommandBytes)));
		if (!lexed.ok || command.size() > kMaxCommandBytes) view.unresolved = true;
		for (const std::vector<Word>& segment : lexed.segments) CollectInvocations(segment, view, depth);
		for (std::string& body : lexed.nested) view.nested.push_back(std::move(body));
		std::vector<std::string> nested = std::move(view.nested);
		view.nested.clear();
		for (const std::string& body : nested)
		{
			if (depth + 1 > kMaxNesting) { view.unresolved = true; continue; }
			Collect(body, view, depth + 1);
		}
	}

	/// <summary>Matches pattern words against the start of an invocation. `**` spans any number of words; the
	/// first word compares as a program name (path, case and .exe ignored) unless the pattern names a path.</summary>
	inline bool MatchWords(const std::vector<Word>& pattern, std::size_t p, const std::vector<Word>& words, std::size_t w, bool program_position)
	{
		if (p == pattern.size()) return true;
		const Word& part = pattern[p];
		if (part.text == "**" && !part.quoted)
		{
			for (std::size_t k = w; k <= words.size(); ++k)
				if (MatchWords(pattern, p + 1, words, k, program_position && k == w)) return true;
			return false;
		}
		if (w >= words.size()) return false;
		bool matched;
		if (program_position && part.text.find_first_of("/\\") == std::string::npos)
			matched = GlobMatch(ProgramName(part.text), ProgramName(words[w].text), true);
		else matched = GlobMatch(part.text, words[w].text);
		return matched && MatchWords(pattern, p + 1, words, w + 1, false);
	}

	inline std::vector<Word> PatternWords(std::string_view pattern)
	{
		Lexed lexed = LexPattern(pattern);
		std::vector<Word> words;
		for (std::vector<Word>& segment : lexed.segments)
			for (Word& word : segment) words.push_back(std::move(word));
		return words;
	}

	/// <summary>Deny/ask side: true when any program invocation anywhere in the command, including wrapped,
	/// chained, substituted and shell -c commands, starts with the pattern.</summary>
	inline bool DenyPatternMatches(const std::vector<Word>& pattern, const CommandView& view)
	{
		if (pattern.empty()) return true;
		return std::any_of(view.invocations.begin(), view.invocations.end(), [&](const std::vector<Word>& words) { return MatchWords(pattern, 0, words, 0, true); });
	}

	/// <summary>Allow side: the command must be one plain simple command with no operators, redirection,
	/// substitution, expansion, glob or unclosed quote; the pattern must match from its very first word (no
	/// leading assignments or wrappers) and the program must be a bare name. Anything else falls through to
	/// the normal flow.</summary>
	inline bool AllowPatternMatches(const std::vector<Word>& pattern, std::string_view command)
	{
		if (pattern.empty() || command.empty() || command.size() > kMaxCommandBytes) return false;
		const Lexed lexed = Lex(command);
		if (!lexed.ok || lexed.compound || lexed.dynamic || lexed.segments.size() != 1) return false;
		const std::vector<Word>& words = lexed.segments.front();
		if (words.empty() || Assignment(words.front()) || words.front().text.find_first_of("/\\") != std::string::npos) return false;
		// Every word must be literal and static, so no expansion can change what the matched words say.
		if (std::any_of(words.begin(), words.end(), [](const Word& word) { return word.dynamic || !word.literal; })) return false;
		// The first word compares exactly (a case-sensitive bare name) on the allow side.
		return MatchWords(pattern, 0, words, 0, false);
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
		if (!rule.provider_id.empty() && rule.provider_id != context.provider_id) return false;
		if (!rule.workspace.empty() && !SamePathOrInside(rule.workspace, context.workspace)) return false;
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
		if (words.empty() || words.front().text == "**" || words.front().text.find_first_of("*?[") != std::string::npos)
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
	    rule("default-env-files", Action::Ask, Tool::Command, "** *.env"),
	    rule("default-env-file-variants", Action::Ask, Tool::Command, "** *.env.*"),
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

/// <summary>True when the rule matches the request (ignoring whether an allow would be permitted).</summary>
inline bool RuleMatches(const Rule& rule, const Context& context, const detail::CommandView* view)
{
	if (!detail::ScopeMatches(rule, context) || !detail::ToolMatches(rule.tool, context)) return false;
	if (rule.tool != Tool::Command) return true;
	const std::vector<detail::Word> pattern = detail::PatternWords(rule.pattern);
	if (rule.action == Action::Allow) return detail::AllowPatternMatches(pattern, context.command);
	return view != nullptr && detail::DenyPatternMatches(pattern, *view);
}

/// <summary>Evaluates every enabled rule. Deny wins over ask, ask over allow. A command whose program is
/// computed at run time (or that does not parse) cannot be proven clear of deny and ask command rules, so
/// it is left for the user whenever such rules exist. An allow match approves only when the caller's
/// ceiling permits; otherwise the normal flow continues.</summary>
inline Decision Evaluate(const std::vector<Rule>& rules, const Context& context)
{
	Decision decision;
	detail::CommandView view;
	const bool command = context.category == Category::Command;
	if (command) detail::Collect(context.command, view, 0);
	if (command && context.command.empty()) view.unresolved = true;

	const Rule* deny = nullptr;
	const Rule* ask = nullptr;
	const Rule* allow = nullptr;
	bool command_guard = false;
	for (const Rule& rule : rules)
	{
		if (!rule.enabled) continue;
		if (rule.action != Action::Allow && (rule.tool == Tool::Command || rule.tool == Tool::Any) && detail::ScopeMatches(rule, context)) command_guard = true;
		if (!RuleMatches(rule, context, command ? &view : nullptr)) continue;
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
	if (ask != nullptr)
	{
		decision.outcome = Outcome::AskUser;
		decision.rule_id = ask->id;
		decision.reason = "An ask rule matched; the request waits for you.";
		return decision;
	}
	if (command && view.unresolved && command_guard)
	{
		decision.outcome = Outcome::AskUser;
		decision.reason = "UAM cannot tell which program this command runs, so deny and ask rules cannot be checked.";
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

/// <summary>For the settings test field: which rule decides a typed command, as if it were a shell command
/// request with every allow permitted (the real ceiling can still refuse an allow at run time).</summary>
inline Decision TestCommand(const std::vector<Rule>& rules, std::string_view command, bool read_only, std::string_view provider_id = {}, std::string_view workspace = {})
{
	Context context;
	context.category = Category::Command;
	context.command = command;
	context.read_only = read_only;
	context.provider_id = provider_id;
	context.workspace = workspace;
	context.allow_permitted = true;
	return Evaluate(rules, context);
}

} // namespace uam::command_rules
