#include "app/provider_agent_import.h"

#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_ids.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <unordered_set>

// Provider names in shared text exist only in builds that include the provider (see docs/build-flags.md).
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
#define UAM_IMPORT_OPENCODE_NAME "OpenCode"
#else
#define UAM_IMPORT_OPENCODE_NAME "the provider"
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#define UAM_IMPORT_GEMINI_NAME "Gemini"
#else
#define UAM_IMPORT_GEMINI_NAME "provider"
#endif

namespace uam::provider_agent_import
{
	namespace
	{
		constexpr std::size_t kMaxFrontmatterBytes = 16U * 1024U;
		constexpr std::size_t kMaxInstructionsBytes = 192U * 1024U;
		constexpr std::uintmax_t kMaxSourceBytes = 256U * 1024U;
		constexpr std::size_t kMaxRuleEntries = 64;
		constexpr std::size_t kMaxRuleKeyBytes = 64;
		constexpr std::size_t kMaxFilesPerFolder = 256;
		constexpr std::uintmax_t kMaxDiscoveryBytes = 8U * 1024U * 1024U;

		/// Tools OpenCode treats as file edits; they are denied together because one permission can govern several.
		constexpr std::array<std::string_view, 5> kEditTools{"edit", "write", "patch", "multiedit", "apply_patch"};

		bool IsEditTool(std::string_view key)
		{
			return std::ranges::find(kEditTools, key) != kEditTools.end();
		}

		std::string_view TrimView(std::string_view value)
		{
			return uam::strings::TrimAsciiView(value);
		}

		std::string Lower(std::string_view value)
		{
			return uam::strings::TrimAndLowerAscii(std::string(value));
		}

		bool WhitespaceOnly(std::string_view value)
		{
			return std::ranges::all_of(value, [](unsigned char ch) { return ch == ' ' || ch == '\t'; });
		}

		bool IsDashLine(std::string_view content)
		{
			return content == "-" || content.starts_with("- ");
		}

		/// Cut a plain scalar at an inline comment (` #`), outside any quotes.
		std::string_view StripPlainComment(std::string_view value)
		{
			for (std::size_t index = 1; index < value.size(); ++index)
			{
				if (value[index] == '#' && (value[index - 1] == ' ' || value[index - 1] == '\t')) return TrimView(value.substr(0, index));
			}
			return TrimView(value);
		}

		bool HasDisallowedControl(std::string_view value)
		{
			return std::ranges::any_of(value, [](unsigned char ch) {
				return ch == 0 || (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t');
			});
		}

		bool SimpleFieldName(std::string_view key)
		{
			return !key.empty() && key.size() <= 64 && std::ranges::all_of(key, [](unsigned char ch) {
				return uam::strings::IsAsciiAlnum(ch) || ch == '-' || ch == '_';
			});
		}

		bool ValidRuleKey(std::string_view key)
		{
			return !key.empty() && key.size() <= kMaxRuleKeyBytes && std::ranges::all_of(key, [](unsigned char ch) {
				return uam::strings::IsAsciiAlnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '*' || ch == '/';
			});
		}

		class FrontmatterParser
		{
		  public:
			explicit FrontmatterParser(const std::vector<std::string>& raw)
			{
				lines_.reserve(raw.size());
				for (std::size_t index = 0; index < raw.size(); ++index)
				{
					Line line;
					line.raw = raw[index];
					line.number = index + 2; // Line 1 is the opening marker.
					while (line.indent < line.raw.size() && line.raw[line.indent] == ' ') ++line.indent;
					line.content = line.raw.substr(line.indent);
					line.tab_indent = !line.content.empty() && line.content.front() == '\t' && !WhitespaceOnly(line.content);
					const std::string_view trimmed = TrimView(line.content);
					line.blank = trimmed.empty() || trimmed.front() == '#';
					lines_.push_back(std::move(line));
				}
			}

			bool Parse(const std::set<std::string>& interpreted, std::vector<std::pair<std::string, Node>>& fields, std::string& error)
			{
				std::set<std::string> seen;
				while (true)
				{
					const std::size_t next = NextContent(at_);
					if (next >= lines_.size()) break;
					const Line& line = lines_[next];
					const std::string where = "frontmatter line " + std::to_string(line.number);
					if (line.tab_indent) return Fail(where, "tabs cannot indent YAML", error);
					if (line.indent != 0) return Fail(where, "unexpected indentation", error);
					if (IsDashLine(line.content)) return Fail(where, "expected a field, not a list item", error);
					at_ = next;
					std::string key;
					std::string_view rest;
					std::string problem;
					if (!SplitKey(line.content, key, rest, problem)) return Fail(where, problem, error);
					if (!SimpleFieldName(key)) return Fail(key.empty() ? where : key, "unsupported field name", error);
					if (!seen.insert(key).second) return Fail(key, "duplicate field", error);
					++at_;
					if (!interpreted.contains(key))
					{
						SkipOpaque(rest);
						Node opaque;
						opaque.kind = Node::Kind::Opaque;
						fields.emplace_back(key, std::move(opaque));
						continue;
					}
					Node node;
					if (!ParseValue(0, rest, key, 1, node)) { error = error_; return false; }
					fields.emplace_back(key, std::move(node));
				}
				return true;
			}

		  private:
			struct Line
			{
				std::string raw;
				std::string content;
				std::size_t indent = 0;
				std::size_t number = 0;
				bool blank = false;
				bool tab_indent = false;
			};

			std::vector<Line> lines_;
			std::size_t at_ = 0;
			std::string error_;

			static bool Fail(const std::string& path, std::string_view problem, std::string& error)
			{
				error = path + ": " + std::string(problem) + ".";
				return false;
			}

			bool Fail(const std::string& path, std::string_view problem)
			{
				return Fail(path, problem, error_);
			}

			std::size_t NextContent(std::size_t from) const
			{
				while (from < lines_.size() && lines_[from].blank) ++from;
				return from;
			}

			static bool SplitKey(std::string_view text, std::string& key, std::string_view& rest, std::string& problem)
			{
				if (text.empty()) { problem = "expected 'key: value'"; return false; }
				if (text.front() == '"' || text.front() == '\'')
				{
					const char quote = text.front();
					std::size_t index = 1;
					bool closed = false;
					key.clear();
					for (; index < text.size(); ++index)
					{
						const char ch = text[index];
						if (quote == '"' && ch == '\\') { problem = "field names cannot use escapes"; return false; }
						if (ch == quote)
						{
							if (quote == '\'' && index + 1 < text.size() && text[index + 1] == '\'') { key.push_back('\''); ++index; continue; }
							closed = true;
							++index;
							break;
						}
						key.push_back(ch);
					}
					if (!closed) { problem = "unclosed quoted key"; return false; }
					while (index < text.size() && text[index] == ' ') ++index;
					if (index >= text.size() || text[index] != ':' || (index + 1 < text.size() && text[index + 1] != ' ' && text[index + 1] != '\t'))
					{
						problem = "expected ':' after the key";
						return false;
					}
					rest = text.substr(index + 1);
					return true;
				}
				if (text.starts_with("? ") || text == "?") { problem = "complex YAML keys are not supported"; return false; }
				if (std::string_view("&*!|>%@`{}[],#").find(text.front()) != std::string_view::npos)
				{
					problem = "YAML anchors, aliases, tags and inline keys are not supported";
					return false;
				}
				for (std::size_t index = 0; index < text.size(); ++index)
				{
					if (text[index] != ':' || (index + 1 < text.size() && text[index + 1] != ' ' && text[index + 1] != '\t')) continue;
					key = std::string(TrimView(text.substr(0, index)));
					rest = text.substr(index + 1);
					if (key.empty()) { problem = "empty key"; return false; }
					if (key == "<<") { problem = "YAML merge keys are not supported"; return false; }
					return true;
				}
				problem = "expected 'key: value'";
				return false;
			}

			/// Skip an uninterpreted field's block without reading it.
			void SkipOpaque(std::string_view rest)
			{
				const std::string_view value = TrimView(rest);
				const bool empty_value = value.empty() || value.front() == '#';
				while (at_ < lines_.size())
				{
					const Line& line = lines_[at_];
					if (line.blank || WhitespaceOnly(line.raw) || line.indent > 0 || line.tab_indent || (empty_value && IsDashLine(line.content))) { ++at_; continue; }
					break;
				}
			}

			bool ParseValue(std::size_t key_indent, std::string_view rest, const std::string& path, std::size_t depth, Node& out)
			{
				std::string_view value = TrimView(rest);
				if (!value.empty() && value.front() == '#') value = {};
				if (value.empty())
				{
					const std::size_t next = NextContent(at_);
					if (next < lines_.size())
					{
						const Line& line = lines_[next];
						const bool dash = IsDashLine(line.content);
						if (line.tab_indent) return Fail(path, "tabs cannot indent YAML");
						if (line.indent > key_indent || (line.indent == key_indent && dash))
						{
							if (depth >= kMaxNestingDepth) return Fail(path, "nesting deeper than " + std::to_string(kMaxNestingDepth) + " levels is not supported");
							return dash ? ParseSequence(line.indent, path, depth, out) : ParseMap(line.indent, path, depth, out);
						}
					}
					out.kind = Node::Kind::Null;
					return true;
				}
				const char first = value.front();
				if (first == '|' || first == '>') return ParseBlockScalar(key_indent, value, path, out);
				if (first == '&' || first == '*' || first == '!') return Fail(path, "YAML anchors, aliases and tags are not supported");
				if (first == '[' || first == '{') return ParseFlow(value, path, out);
				if (first == '"' || first == '\'') return ParseQuoted(key_indent, value, path, out);
				if (first == '%' || first == '@' || first == '`') return Fail(path, "reserved YAML indicator");
				if (IsDashLine(value)) return Fail(path, "list items must start on their own line");
				return ParsePlain(key_indent, value, path, out);
			}

			bool ParseMap(std::size_t indent, const std::string& path, std::size_t depth, Node& out)
			{
				out.kind = Node::Kind::Map;
				std::set<std::string> seen;
				while (true)
				{
					const std::size_t next = NextContent(at_);
					if (next >= lines_.size() || lines_[next].indent < indent) break;
					const Line& line = lines_[next];
					if (line.tab_indent) return Fail(path, "tabs cannot indent YAML");
					if (line.indent > indent) return Fail(path, "unexpected indentation");
					if (IsDashLine(line.content)) return Fail(path, "mixes list items and keys");
					at_ = next;
					std::string key;
					std::string_view rest;
					std::string problem;
					if (!SplitKey(line.content, key, rest, problem)) return Fail(path, problem);
					const std::string child = path + "." + key;
					if (!seen.insert(key).second) return Fail(child, "duplicate key");
					if (out.entries.size() >= kMaxCollectionEntries) return Fail(path, "too many entries");
					++at_;
					Node node;
					if (!ParseValue(indent, rest, child, depth + 1, node)) return false;
					out.entries.emplace_back(std::move(key), std::move(node));
				}
				return true;
			}

			static bool LooksLikeKey(std::string_view item)
			{
				if (item.empty() || item.front() == '"' || item.front() == '\'' || item.front() == '[' || item.front() == '{') return false;
				return item.find(": ") != std::string_view::npos || item.ends_with(':');
			}

			bool ParseSequence(std::size_t indent, const std::string& path, std::size_t depth, Node& out)
			{
				out.kind = Node::Kind::Sequence;
				while (true)
				{
					const std::size_t next = NextContent(at_);
					if (next >= lines_.size() || lines_[next].indent < indent) break;
					const Line& line = lines_[next];
					if (line.tab_indent) return Fail(path, "tabs cannot indent YAML");
					if (line.indent > indent) return Fail(path, "unexpected indentation");
					if (!IsDashLine(line.content)) break;
					at_ = next;
					const std::string child = path + "[" + std::to_string(out.items.size()) + "]";
					const std::string_view item = line.content.size() > 1 ? TrimView(std::string_view(line.content).substr(2)) : std::string_view{};
					if (item.empty() || item.front() == '#') return Fail(child, "nested list items are not supported");
					if (LooksLikeKey(item)) return Fail(child, "list items with keys are not supported");
					if (out.items.size() >= kMaxCollectionEntries) return Fail(path, "too many entries");
					++at_;
					Node node;
					if (!ParseValue(indent, item, child, depth + 1, node)) return false;
					if (node.kind != Node::Kind::Scalar) return Fail(child, "nested list items are not supported");
					out.items.push_back(std::move(node));
				}
				return true;
			}

			bool ParsePlain(std::size_t key_indent, std::string_view value, const std::string& path, Node& out)
			{
				std::string text(StripPlainComment(value));
				while (true)
				{
					std::size_t next = at_;
					std::size_t blanks = 0;
					while (next < lines_.size() && lines_[next].blank) { ++next; ++blanks; }
					if (next >= lines_.size() || lines_[next].indent <= key_indent) break;
					if (lines_[next].tab_indent) return Fail(path, "tabs cannot indent YAML");
					// A folded line that looks like a key is invalid YAML and could hide a rule inside plain text.
					if (LooksLikeKey(TrimView(lines_[next].content))) return Fail(path, "a continuation line looks like a key; quote the value or use a | block");
					text += blanks > 0 ? std::string(blanks, '\n') : std::string(" ");
					text += StripPlainComment(lines_[next].content);
					at_ = next + 1;
				}
				out.kind = Node::Kind::Scalar;
				out.text = std::string(TrimView(text));
				out.quoted = false;
				return true;
			}

			bool DecodeQuotedLine(char quote, std::string_view line, std::string& value, bool& closed, std::size_t& end, const std::string& path)
			{
				closed = false;
				for (std::size_t index = 0; index < line.size(); ++index)
				{
					const char ch = line[index];
					if (quote == '"' && ch == '\\')
					{
						if (index + 1 >= line.size()) return Fail(path, "line-continuation escapes are not supported");
						const char escaped = line[++index];
						switch (escaped)
						{
						case '"': value.push_back('"'); break;
						case '\\': value.push_back('\\'); break;
						case '/': value.push_back('/'); break;
						case 'n': value.push_back('\n'); break;
						case 't': value.push_back('\t'); break;
						default: return Fail(path, std::string("unsupported escape \\") + escaped);
						}
						continue;
					}
					if (ch == quote)
					{
						if (quote == '\'' && index + 1 < line.size() && line[index + 1] == '\'') { value.push_back('\''); ++index; continue; }
						closed = true;
						end = index + 1;
						return true;
					}
					value.push_back(ch);
				}
				return true;
			}

			bool ParseQuoted(std::size_t key_indent, std::string_view value, const std::string& path, Node& out)
			{
				const char quote = value.front();
				std::string decoded;
				std::string_view line = value.substr(1);
				while (true)
				{
					bool closed = false;
					std::size_t end = 0;
					if (!DecodeQuotedLine(quote, line, decoded, closed, end, path)) return false;
					if (closed)
					{
						const std::string_view trailing = TrimView(line.substr(end));
						if (!trailing.empty() && trailing.front() != '#') return Fail(path, "unexpected text after the quoted value");
						break;
					}
					while (!decoded.empty() && (decoded.back() == ' ' || decoded.back() == '\t')) decoded.pop_back();
					std::size_t next = at_;
					std::size_t blanks = 0;
					while (next < lines_.size() && WhitespaceOnly(lines_[next].raw)) { ++next; ++blanks; }
					if (next >= lines_.size() || lines_[next].indent <= key_indent) return Fail(path, "unclosed quoted value");
					decoded += blanks > 0 ? std::string(blanks, '\n') : std::string(" ");
					line = TrimView(lines_[next].raw);
					at_ = next + 1;
				}
				out.kind = Node::Kind::Scalar;
				out.text = std::move(decoded);
				out.quoted = true;
				return true;
			}

			bool ParseBlockScalar(std::size_t key_indent, std::string_view header, const std::string& path, Node& out)
			{
				const char style = header.front();
				char chomp = 'c';
				std::size_t explicit_indent = 0;
				for (std::size_t index = 1; index < header.size(); ++index)
				{
					const char ch = header[index];
					if ((ch == '-' || ch == '+') && chomp == 'c') chomp = ch;
					else if (ch >= '1' && ch <= '9' && explicit_indent == 0) explicit_indent = static_cast<std::size_t>(ch - '0');
					else if (ch == ' ' || ch == '\t')
					{
						const std::string_view trailing = TrimView(header.substr(index));
						if (!trailing.empty() && trailing.front() != '#') return Fail(path, "invalid block scalar header");
						break;
					}
					else return Fail(path, "invalid block scalar header");
				}
				std::vector<std::string_view> content;
				std::size_t content_indent = explicit_indent == 0 ? 0 : key_indent + explicit_indent;
				std::size_t next = at_;
				while (next < lines_.size())
				{
					const Line& line = lines_[next];
					if (WhitespaceOnly(line.raw)) { content.emplace_back(); ++next; continue; }
					if (line.indent <= key_indent) break;
					if (content_indent == 0) content_indent = line.indent;
					if (line.indent < content_indent) return Fail(path, "inconsistent block indentation");
					content.push_back(std::string_view(line.raw).substr(content_indent));
					++next;
				}
				at_ = next;
				std::size_t last = content.size();
				while (last > 0 && content[last - 1].empty()) --last;
				std::string text;
				for (std::size_t index = 0; index < last; ++index)
				{
					const std::string_view line = content[index];
					if (index == 0) { text += line; continue; }
					const std::string_view previous = content[index - 1];
					if (style == '|') { text += '\n'; text += line; continue; }
					if (line.empty()) text += '\n';
					else if (previous.empty()) text += line;
					else if (previous.front() == ' ' || line.front() == ' ') { text += '\n'; text += line; }
					else { text += ' '; text += line; }
				}
				if (last > 0 && chomp == 'c') text += '\n';
				if (last > 0 && chomp == '+') text += std::string(content.size() - last + 1, '\n');
				out.kind = Node::Kind::Scalar;
				out.text = std::move(text);
				out.quoted = true;
				return true;
			}

			/// Split a one-line flow collection body on commas outside quotes.
			bool SplitFlow(std::string_view inner, const std::string& path, std::vector<std::string_view>& parts)
			{
				char quote = 0;
				std::size_t start = 0;
				for (std::size_t index = 0; index < inner.size(); ++index)
				{
					const char ch = inner[index];
					if (quote != 0)
					{
						if (quote == '"' && ch == '\\') { ++index; continue; }
						if (ch == quote) quote = 0;
						continue;
					}
					if (ch == '"' || ch == '\'') { quote = ch; continue; }
					if (ch == '[' || ch == '{' || ch == ']' || ch == '}') return Fail(path, "nested inline lists or maps are not supported");
					if (ch == ',') { parts.push_back(TrimView(inner.substr(start, index - start))); start = index + 1; }
				}
				if (quote != 0) return Fail(path, "unclosed quoted value");
				parts.push_back(TrimView(inner.substr(start)));
				if (!parts.empty() && parts.back().empty()) parts.pop_back(); // trailing comma
				if (std::ranges::any_of(parts, [](std::string_view part) { return part.empty(); })) return Fail(path, "empty inline entry");
				if (parts.size() > kMaxCollectionEntries) return Fail(path, "too many entries");
				return true;
			}

			bool FlowScalar(std::string_view text, const std::string& path, Node& out)
			{
				text = TrimView(text);
				out.kind = Node::Kind::Scalar;
				if (text.empty()) { out.kind = Node::Kind::Null; return true; }
				if (text.front() == '"' || text.front() == '\'')
				{
					std::string decoded;
					bool closed = false;
					std::size_t end = 0;
					if (!DecodeQuotedLine(text.front(), text.substr(1), decoded, closed, end, path)) return false;
					if (!closed || !TrimView(text.substr(1 + end)).empty()) return Fail(path, "malformed quoted value");
					out.text = std::move(decoded);
					out.quoted = true;
					return true;
				}
				if (std::string_view("&*!|>%@`").find(text.front()) != std::string_view::npos) return Fail(path, "YAML anchors, aliases and tags are not supported");
				if (text.find(": ") != std::string_view::npos) return Fail(path, "inline maps inside lists are not supported");
				out.text = std::string(text);
				out.quoted = false;
				return true;
			}

			bool ParseFlow(std::string_view value, const std::string& path, Node& out)
			{
				// Find the end of the collection outside quotes, then allow only a trailing comment.
				char quote = 0;
				std::size_t close = std::string_view::npos;
				for (std::size_t index = 0; index < value.size(); ++index)
				{
					const char ch = value[index];
					if (quote != 0)
					{
						if (quote == '"' && ch == '\\') { ++index; continue; }
						if (ch == quote) quote = 0;
						continue;
					}
					if (ch == '"' || ch == '\'') quote = ch;
					else if (ch == '#' && index > 0 && (value[index - 1] == ' ' || value[index - 1] == '\t')) break;
					else if (ch == ']' || ch == '}') close = index;
				}
				const char open = value.front();
				const char expected = open == '[' ? ']' : '}';
				if (close == std::string_view::npos || value[close] != expected) return Fail(path, "inline lists or maps must close on the same line");
				const std::string_view trailing = TrimView(value.substr(close + 1));
				if (!trailing.empty() && trailing.front() != '#') return Fail(path, "unexpected text after the inline value");
				std::vector<std::string_view> parts;
				if (!SplitFlow(value.substr(1, close - 1), path, parts)) return false;
				if (open == '[')
				{
					out.kind = Node::Kind::Sequence;
					for (const std::string_view part : parts)
					{
						Node item;
						if (!FlowScalar(part, path + "[" + std::to_string(out.items.size()) + "]", item)) return false;
						out.items.push_back(std::move(item));
					}
					return true;
				}
				out.kind = Node::Kind::Map;
				std::set<std::string> seen;
				for (const std::string_view part : parts)
				{
					std::string key;
					std::string_view rest;
					std::string problem;
					if (!SplitKey(part, key, rest, problem))
					{
						// `{read}` style flow sets or `key:value` without a space are outside the subset.
						return Fail(path, problem);
					}
					const std::string child = path + "." + key;
					if (!seen.insert(key).second) return Fail(child, "duplicate key");
					Node node;
					if (!FlowScalar(rest, child, node)) return false;
					out.entries.emplace_back(std::move(key), std::move(node));
				}
				return true;
			}
		};

		using OrderedJson = nlohmann::ordered_json;

		/// JSON(C) with per-object duplicate-key detection; returns a discarded value on failure.
		OrderedJson ParseJsonc(const std::string& text, std::string& error)
		{
			const std::string stripped = StripJsonc(text);
			std::vector<std::set<std::string>> keys;
			std::string duplicate;
			OrderedJson parsed = OrderedJson::parse(stripped,
			    [&](int, OrderedJson::parse_event_t event, OrderedJson& value)
			    {
				    if (event == OrderedJson::parse_event_t::object_start) keys.emplace_back();
				    else if (event == OrderedJson::parse_event_t::object_end && !keys.empty()) keys.pop_back();
				    else if (event == OrderedJson::parse_event_t::key && !keys.empty() && !keys.back().insert(value.get<std::string>()).second && duplicate.empty())
					    duplicate = value.get<std::string>();
				    return true;
			    }, false);
			if (parsed.is_discarded()) { error = "config: not valid JSON or JSONC."; return OrderedJson(OrderedJson::value_t::discarded); }
			if (!duplicate.empty()) { error = "config: duplicate key " + duplicate + "."; return OrderedJson(OrderedJson::value_t::discarded); }
			return parsed;
		}

		bool JsonToNode(const OrderedJson& value, const std::string& path, std::size_t depth, Node& out, std::string& error)
		{
			if (value.is_string()) { out.kind = Node::Kind::Scalar; out.text = value.get<std::string>(); out.quoted = true; return true; }
			if (value.is_boolean()) { out.kind = Node::Kind::Scalar; out.text = value.get<bool>() ? "true" : "false"; return true; }
			if (value.is_number()) { out.kind = Node::Kind::Scalar; out.text = value.dump(); return true; }
			if (value.is_null()) { out.kind = Node::Kind::Null; return true; }
			if (depth >= kMaxNestingDepth) { error = path + ": nesting deeper than " + std::to_string(kMaxNestingDepth) + " levels is not supported."; return false; }
			if (value.size() > kMaxCollectionEntries) { error = path + ": too many entries."; return false; }
			if (value.is_array())
			{
				out.kind = Node::Kind::Sequence;
				for (const OrderedJson& item : value)
				{
					Node node;
					if (!JsonToNode(item, path + "[" + std::to_string(out.items.size()) + "]", depth + 1, node, error)) return false;
					out.items.push_back(std::move(node));
				}
				return true;
			}
			out.kind = Node::Kind::Map;
			for (const auto& [key, item] : value.items())
			{
				Node node;
				if (!JsonToNode(item, path + "." + key, depth + 1, node, error)) return false;
				out.entries.emplace_back(key, std::move(node));
			}
			return true;
		}

		/// Read a regular, non-linked text file within a byte bound.
		bool ReadBoundedFile(const std::filesystem::path& path, std::uintmax_t limit, std::string& text)
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(path);
			if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
			    uam::paths::IsLinkOrReparsePointNoThrow(path) || !size.has_value() || *size == 0 || *size > limit)
				return false;
			return uam::io::TryReadTextFile(path, text) && text.size() == *size && !HasDisallowedControl(text);
		}

		std::filesystem::path OpenCodeConfigRoot()
		{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
			if (const auto xdg = uam::env::GetNonEmptyString("XDG_CONFIG_HOME")) return uam::paths::PathFromUtf8(*xdg) / "opencode";
			if (const auto home = PlatformServicesFactory::Instance().path_service.ResolveUserHomePath()) return *home / ".config" / "opencode";
#endif
			return {};
		}

		std::string ProviderLabel(const std::string& provider_id)
		{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
			if (provider_id == provider_ids::kOpenCodeCli) return "OpenCode";
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
			if (provider_id == provider_ids::kCopilotCli) return "Copilot";
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
			if (provider_id == provider_ids::kGeminiCli) return "Gemini";
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
			if (provider_id == provider_ids::kClaudeCli) return "Claude";
#endif
			(void)provider_id;
			return "provider";
		}

		std::string ProviderPrefix(const std::string& provider_id)
		{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
			if (provider_id == provider_ids::kOpenCodeCli) return "opencode";
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
			if (provider_id == provider_ids::kCopilotCli) return "copilot";
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
			if (provider_id == provider_ids::kGeminiCli) return "gemini";
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
			if (provider_id == provider_ids::kClaudeCli) return "claude";
#endif
			(void)provider_id;
			return "imported";
		}

		int MajorVersion(std::string_view version)
		{
			version = TrimView(version);
			if (version.starts_with('v') || version.starts_with('V')) version.remove_prefix(1);
			int major = 0;
			std::size_t digits = 0;
			while (digits < version.size() && digits < 6 && uam::strings::IsAsciiDigit(static_cast<unsigned char>(version[digits])))
				major = major * 10 + (version[digits++] - '0');
			return digits == 0 ? -1 : major;
		}

		struct ToolVocabulary
		{
			std::set<std::string> read_only;
			std::set<std::string> write;
		};

		/// Lowercased native tool names; anything unknown counts as able to change the workspace.
		ToolVocabulary VocabularyFor(const std::string& provider_id)
		{
			if (provider_id == provider_ids::kClaudeCli)
				return {{"read", "grep", "glob", "ls", "webfetch", "websearch", "todowrite", "todoread", "notebookread", "exitplanmode"},
				        {"write", "edit", "multiedit", "notebookedit", "bash"}};
			if (provider_id == provider_ids::kGeminiCli)
				return {{"read_file", "read_many_files", "glob", "search_file_content", "grep", "list_directory", "ls", "google_web_search", "web_fetch", "write_todos"},
				        {"write_file", "replace", "edit", "run_shell_command"}};
			return {{"read", "search", "view", "grep", "glob", "web", "fetch", "todo"},
			        {"edit", "write", "shell", "execute", "bash"}};
		}

		bool ToolNames(const Node* node, const std::string& field, std::vector<std::string>& names, std::string& error)
		{
			if (node == nullptr || node->kind == Node::Kind::Null) return true;
			const auto add = [&](std::string_view raw)
			{
				std::string name = Lower(raw);
				if (const std::size_t paren = name.find('('); paren != std::string::npos) name = Lower(name.substr(0, paren));
				if (!name.empty()) names.push_back(std::move(name));
			};
			if (node->kind == Node::Kind::Scalar)
			{
				std::string_view text = node->text;
				std::size_t start = 0;
				while (start <= text.size())
				{
					const std::size_t comma = text.find(',', start);
					add(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
					if (comma == std::string_view::npos) break;
					start = comma + 1;
				}
				return true;
			}
			if (node->kind == Node::Kind::Sequence)
			{
				for (const Node& item : node->items)
				{
					if (item.kind != Node::Kind::Scalar) { error = field + ": expected a list of tool names."; return false; }
					add(item.text);
				}
				return true;
			}
			error = field + ": expected a list of tool names.";
			return false;
		}

		std::string Join(const std::vector<std::string>& values, std::string_view separator)
		{
			std::string joined;
			for (const std::string& value : values)
			{
				if (!joined.empty()) joined += separator;
				joined += value;
			}
			return joined;
		}

		void AddNote(std::vector<std::string>& notes, std::string note)
		{
			if (std::ranges::find(notes, note) == notes.end()) notes.push_back(std::move(note));
		}

		/// Effect of an OpenCode rule value; `allow` stays mediated by UAM, so it becomes `ask`.
		std::optional<std::string> RuleEffect(const Node& node)
		{
			if (node.kind != Node::Kind::Scalar) return std::nullopt;
			const std::string value = Lower(node.text);
			if (value == "allow" || value == "ask") return std::string("ask");
			if (value == "deny") return std::string("deny");
			return std::nullopt;
		}

		bool IsWildcard(std::string_view key)
		{
			return key.find('*') != std::string_view::npos;
		}

		/// `*` matches any run of characters; every other character matches itself.
		bool GlobMatch(std::string_view pattern, std::string_view text)
		{
			std::size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
			while (t < text.size())
			{
				if (p < pattern.size() && pattern[p] != '*' && pattern[p] == text[t]) { ++p; ++t; }
				else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
				else if (star != std::string_view::npos) { p = star + 1; t = ++mark; }
				else return false;
			}
			while (p < pattern.size() && pattern[p] == '*') ++p;
			return p == pattern.size();
		}

		bool Covers(std::string_view key, std::string_view name)
		{
			return key == name || (IsWildcard(key) && GlobMatch(key, name));
		}

		struct Rule
		{
			std::string key;
			std::string effect;
			std::string origin;
		};

		/// OpenCode evaluates rules in order and the last matching rule wins.
		const Rule* LastMatch(const std::vector<Rule>& rules, std::string_view name)
		{
			const Rule* match = nullptr;
			for (const Rule& rule : rules)
				if (Covers(rule.key, name)) match = &rule;
			return match;
		}

		/// The last rule that decides a wildcard key itself: the same key or a later `*`.
		const Rule* LastPatternMatch(const std::vector<Rule>& rules, std::string_view key)
		{
			const Rule* match = nullptr;
			for (const Rule& rule : rules)
				if (rule.key == key || rule.key == "*") match = &rule;
			return match;
		}

		struct Verdict
		{
			std::string effect = "ask";
			std::string origin;
		};

		/// Overlay the agent's rule map onto the config's in place (OpenCode's key-by-key merge): a repeated key keeps its
		/// first position and takes the agent's value; a non-map on either side lets the agent's value replace the config's.
		Node Overlay(const Node& base, const Node& over)
		{
			if (over.kind == Node::Kind::Null) return base;
			if (base.kind != Node::Kind::Map || over.kind != Node::Kind::Map) return over;
			Node merged = base;
			for (const auto& [key, value] : over.entries)
			{
				const auto found = std::ranges::find_if(merged.entries, [&](const auto& entry) { return entry.first == key; });
				if (found == merged.entries.end()) merged.entries.emplace_back(key, value);
				else found->second = Overlay(found->second, value);
			}
			return merged;
		}

		/// Strictest verdict for one name across every reading; `tools: false` disables a tool whatever `permission` says.
		Verdict VerdictFor(const std::vector<const std::vector<Rule>*>& lists, std::string_view name, bool pattern)
		{
			Verdict verdict;
			for (const std::vector<Rule>* list : lists)
			{
				const Rule* rule = pattern ? LastPatternMatch(*list, name) : LastMatch(*list, name);
				if (rule == nullptr) continue;
				if (verdict.origin.empty() || (rule->effect == "deny" && verdict.effect != "deny")) verdict = {rule->effect, rule->origin};
			}
			return verdict;
		}

		/// Last-match effect of `name` under a stored policy, as OpenCode evaluates it.
		std::string StoredEffect(const OrderedJson& policy, std::string_view name)
		{
			std::string effect = "ask";
			for (const auto& [key, value] : policy.items())
				if (value.is_string() && Covers(key, name)) effect = value.get<std::string>();
			return effect;
		}

		struct NormalizedRules
		{
			OrderedJson canonical = OrderedJson::object();
			bool explicit_write = false;
		};

		bool AddToolRules(const Node& tools, const std::string& prefix, std::vector<Rule>& out, bool& explicit_write, ProviderAgentImportPreview& preview)
		{
			if (tools.kind == Node::Kind::Null) return true;
			if (tools.kind != Node::Kind::Map) { preview.error = prefix + ": expected a map of tool: true or false."; return false; }
			for (const auto& [key, value] : tools.entries)
			{
				const std::string path = prefix + "." + key;
				if (!ValidRuleKey(key)) { preview.error = path + ": unsupported tool name."; return false; }
				const std::string flag = value.kind == Node::Kind::Scalar ? Lower(value.text) : std::string{};
				if (flag != "true" && flag != "false") { preview.error = path + ": expected true or false."; return false; }
				out.push_back({key, flag == "true" ? "ask" : "deny", path});
				if (flag == "true" && IsEditTool(key)) explicit_write = true;
			}
			AddNote(preview.notes, "tools: enabled tools become ask and disabled tools become deny.");
			return true;
		}

		bool AddPermissionRules(const Node& permission, const std::string& prefix, std::vector<Rule>& out, bool& explicit_write, ProviderAgentImportPreview& preview)
		{
			if (permission.kind == Node::Kind::Null) return true;
			if (permission.kind == Node::Kind::Scalar)
			{
				const std::optional<std::string> effect = RuleEffect(permission);
				if (!effect) { preview.error = prefix + ": expected allow, ask or deny."; return false; }
				out.push_back({"*", *effect, prefix});
				if (Lower(permission.text) == "allow") explicit_write = true;
				return true;
			}
			if (permission.kind != Node::Kind::Map) { preview.error = prefix + ": expected allow, ask, deny or a map of tool rules."; return false; }
			for (const auto& [key, value] : permission.entries)
			{
				const std::string path = prefix + "." + key;
				if (!ValidRuleKey(key)) { preview.error = path + ": unsupported tool name."; return false; }
				if (value.kind == Node::Kind::Map)
				{
					if (value.entries.empty()) { preview.error = path + ": empty rule map."; return false; }
					bool any_deny = false;
					bool all_deny = true;
					for (const auto& [pattern, rule] : value.entries)
					{
						const std::optional<std::string> effect = RuleEffect(rule);
						if (!effect) { preview.error = path + "." + pattern + ": expected allow, ask or deny."; return false; }
						any_deny = any_deny || *effect == "deny";
						all_deny = all_deny && *effect == "deny";
					}
					if (any_deny && !all_deny)
						AddNote(preview.notes, path + ": pattern rules become one rule, so a deny for any pattern denies the whole tool.");
					out.push_back({key, any_deny ? "deny" : "ask", path});
					if (!any_deny && IsEditTool(key)) explicit_write = true;
					continue;
				}
				const std::optional<std::string> effect = RuleEffect(value);
				if (!effect) { preview.error = path + ": expected allow, ask or deny."; return false; }
				out.push_back({key, *effect, path});
				if (*effect == "ask" && IsEditTool(key)) explicit_write = true;
				if (Lower(value.text) == "allow") AddNote(preview.notes, "permission: allow becomes ask, so UAM still asks before each use.");
			}
			return true;
		}

		/// Resolve inherited and agent rules in OpenCode's order (last match wins) into a canonical policy whose own
		/// last-match reading is never looser than the source: `*` first, then other wildcards, then exact tools.
		bool NormalizeToolRules(const ParsedSource& source, ProviderAgentImportPreview& preview, NormalizedRules& out)
		{
			// Config rules reach an agent either appended before its own rules or merged with them key by key; both
			// readings are computed and the strictest verdict is kept, so neither merge model can make the import looser.
			Node no_rules;
			const Node* inherited_tools = &no_rules;
			const Node* inherited_permission = &no_rules;
			for (const InheritedRules& inherited : source.inherited)
			{
				(inherited.field == "tools" ? inherited_tools : inherited_permission) = &inherited.node;
				AddNote(preview.notes, "config: " + inherited.field + " rules from " + inherited.files + " apply before the agent's own rules.");
			}
			const Node* own_tools = source.Find("tools");
			const Node* own_permission = source.Find("permission");
			if (own_tools == nullptr) own_tools = &no_rules;
			if (own_permission == nullptr) own_permission = &no_rules;
			std::vector<Rule> permission_appended, permission_merged, tools_appended, tools_merged;
			bool explicit_write = false;
			bool own_write = false;
			if (!AddToolRules(*inherited_tools, "config tools", tools_appended, explicit_write, preview) ||
			    !AddToolRules(*own_tools, "tools", tools_appended, own_write, preview) ||
			    !AddToolRules(Overlay(*inherited_tools, *own_tools), "tools", tools_merged, own_write, preview) ||
			    !AddPermissionRules(*inherited_permission, "config permission", permission_appended, explicit_write, preview) ||
			    !AddPermissionRules(*own_permission, "permission", permission_appended, own_write, preview) ||
			    !AddPermissionRules(Overlay(*inherited_permission, *own_permission), "permission", permission_merged, own_write, preview))
				return false;
			explicit_write = explicit_write || own_write;
			const std::vector<const std::vector<Rule>*> lists{&permission_appended, &permission_merged, &tools_appended, &tools_merged};
			if (std::ranges::all_of(lists, [](const auto* list) { return list->empty(); })) return true;
			const auto verdict_for = [&](std::string_view name, bool pattern = false) { return VerdictFor(lists, name, pattern); };

			std::set<std::string> exact;
			std::set<std::string> wildcards;
			for (const std::vector<Rule>* list : lists)
				for (const Rule& rule : *list) (IsWildcard(rule.key) ? wildcards : exact).insert(rule.key);
			std::map<std::string, Verdict> wildcard_verdicts;
			for (const std::string& key : wildcards) wildcard_verdicts[key] = verdict_for(key, true);
			// Wildcards other than `*` that disagree cannot keep a safe order between them, so both deny.
			bool conflicting = false;
			for (auto& [key, verdict] : wildcard_verdicts)
				for (const auto& [other, other_verdict] : wildcard_verdicts)
					if (key != "*" && other != "*" && key != other && verdict.effect != "deny" && other_verdict.effect == "deny")
					{
						verdict = {"deny", other_verdict.origin};
						conflicting = true;
					}
			if (conflicting) AddNote(preview.notes, "permission: wildcard rules that disagree are both denied, because their order cannot be kept.");
			OrderedJson canonical = OrderedJson::object();
			if (wildcard_verdicts.contains("*")) canonical["*"] = wildcard_verdicts["*"].effect;
			for (const auto& [key, verdict] : wildcard_verdicts)
				if (key != "*") canonical[key] = verdict.effect;
			std::map<std::string, Verdict> exact_verdicts;
			for (const std::string& key : exact)
			{
				exact_verdicts[key] = verdict_for(key);
				canonical[key] = exact_verdicts[key].effect;
			}
			if (canonical.size() > kMaxRuleEntries) { preview.error = "permission: more than 64 tool rules."; return false; }

			// OpenCode checks file edits under `edit`; a denied edit tool denies them all and caps access at read.
			std::string reason;
			if (const Verdict edit = verdict_for("edit"); edit.effect == "deny") reason = edit.origin;
			for (const std::string_view tool : kEditTools)
			{
				if (!reason.empty()) break;
				const bool named = exact.contains(std::string(tool)) ||
				                   std::ranges::any_of(wildcards, [&](const std::string& key) { return key != "*" && GlobMatch(key, tool); });
				if (const Verdict verdict = verdict_for(tool); named && verdict.effect == "deny") reason = verdict.origin;
			}
			if (!reason.empty())
			{
				bool tightened = false;
				for (const std::string_view tool : kEditTools)
				{
					const std::string key(tool);
					if (key != "edit" && !canonical.contains(key)) continue;
					if (canonical.contains(key) && canonical[key] != "deny") tightened = true;
					canonical[key] = "deny";
				}
				if (tightened) AddNote(preview.notes, "edit tools: " UAM_IMPORT_OPENCODE_NAME " may check file edits under one rule, so every edit tool is denied.");
				preview.max_workspace_access = "read";
				preview.read_only_reason = reason + " is deny";
			}
			// Leave room for the edit deny a read import adds.
			if (canonical.size() + (canonical.contains("edit") ? 0 : 1) > kMaxRuleEntries) { preview.error = "permission: more than 63 tool rules."; return false; }
			// The canonical reading must never be looser than the source for any named tool.
			for (const auto& [key, verdict] : exact_verdicts)
				if (verdict.effect == "deny" && StoredEffect(canonical, key) != "deny") canonical[key] = "deny";
			out.canonical = std::move(canonical);
			out.explicit_write = explicit_write && preview.max_workspace_access == "write";
			return true;
		}
	} // namespace

	Node::Node() = default;
	Node::Node(const Node&) = default;
	Node::Node(Node&&) noexcept = default;
	Node& Node::operator=(const Node&) = default;
	Node& Node::operator=(Node&&) noexcept = default;
	Node::~Node() = default;

	const Node* Node::Find(std::string_view key) const
	{
		const auto found = std::ranges::find_if(entries, [&](const auto& entry) { return entry.first == key; });
		return found == entries.end() ? nullptr : &found->second;
	}

	const Node* ParsedSource::Find(std::string_view key) const
	{
		const auto found = std::ranges::find_if(fields, [&](const auto& entry) { return entry.first == key; });
		return found == fields.end() ? nullptr : &found->second;
	}

	bool ParseFrontmatter(const std::vector<std::string>& lines, const std::set<std::string>& interpreted,
	                      std::vector<std::pair<std::string, Node>>& fields, std::string& error)
	{
		FrontmatterParser parser(lines);
		return parser.Parse(interpreted, fields, error);
	}

	bool ParseMarkdownAgent(const std::string& text, const std::set<std::string>& interpreted, ParsedSource& out)
	{
		std::size_t position = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;
		const auto next_line = [&](std::string& line) -> bool
		{
			if (position >= text.size()) return false;
			const std::size_t end = text.find('\n', position);
			line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
			position = end == std::string::npos ? text.size() : end + 1;
			if (line.ends_with('\r')) line.pop_back();
			return true;
		};
		// YAML and OpenCode's loader treat a bare CR as a line break; splitting only on LF would hide a field.
		for (std::size_t index = 0; index < text.size(); ++index)
			if (text[index] == '\r' && (index + 1 == text.size() || text[index + 1] != '\n'))
			{
				out.error = "frontmatter: a carriage return without a line feed is not supported; save the file with LF or CRLF line endings.";
				return false;
			}
		std::string line;
		if (!next_line(line) || TrimView(line) != "---")
		{
			out.error = "frontmatter: the file must start with a --- line.";
			return false;
		}
		std::vector<std::string> lines;
		std::size_t bytes = 4;
		bool closed = false;
		while (next_line(line))
		{
			bytes += line.size() + 1;
			if (bytes > kMaxFrontmatterBytes) { out.error = "frontmatter: larger than 16 KiB."; return false; }
			std::string marker = line;
			while (!marker.empty() && (marker.back() == ' ' || marker.back() == '\t')) marker.pop_back();
			if (marker == "---") { closed = true; break; }
			lines.push_back(line);
		}
		if (!closed) { out.error = "frontmatter: missing the closing --- line."; return false; }
		if (!ParseFrontmatter(lines, interpreted, out.fields, out.error)) return false;
		out.instructions = uam::strings::Trim(text.substr(std::min(position, text.size())));
		return true;
	}

	std::string StripJsonc(std::string_view text)
	{
		std::string stripped;
		stripped.reserve(text.size());
		bool in_string = false;
		bool escaped = false;
		for (std::size_t index = 0; index < text.size(); ++index)
		{
			const char ch = text[index];
			if (in_string)
			{
				stripped.push_back(ch);
				if (escaped) escaped = false;
				else if (ch == '\\') escaped = true;
				else if (ch == '"') in_string = false;
				continue;
			}
			if (ch == '"') { in_string = true; stripped.push_back(ch); continue; }
			if (ch == '/' && index + 1 < text.size() && text[index + 1] == '/')
			{
				while (index < text.size() && text[index] != '\n') ++index;
				if (index < text.size()) stripped.push_back('\n');
				continue;
			}
			if (ch == '/' && index + 1 < text.size() && text[index + 1] == '*')
			{
				index += 2;
				while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/'))
				{
					if (text[index] == '\n') stripped.push_back('\n');
					++index;
				}
				++index;
				continue;
			}
			stripped.push_back(ch);
		}
		std::string result;
		result.reserve(stripped.size());
		in_string = false;
		escaped = false;
		for (std::size_t index = 0; index < stripped.size(); ++index)
		{
			const char ch = stripped[index];
			if (in_string)
			{
				result.push_back(ch);
				if (escaped) escaped = false;
				else if (ch == '\\') escaped = true;
				else if (ch == '"') in_string = false;
				continue;
			}
			if (ch == '"') in_string = true;
			if (ch == ',')
			{
				std::size_t next = index + 1;
				while (next < stripped.size() && uam::strings::IsAsciiSpace(static_cast<unsigned char>(stripped[next]))) ++next;
				if (next < stripped.size() && (stripped[next] == '}' || stripped[next] == ']')) continue;
			}
			result.push_back(ch);
		}
		return result;
	}

	bool ParseJsonConfigAgent(const std::string& text, const std::string& entry,
	                            const std::filesystem::path& config_path, ParsedSource& out)
	{
		std::string error;
		const OrderedJson root = ParseJsonc(text, error);
		if (root.is_discarded()) { out.error = error; return false; }
		if (!root.is_object()) { out.error = "config: expected a JSON object."; return false; }
		const OrderedJson* agent = nullptr;
		for (const char* container : {"agent", "agents"})
		{
			if (!root.contains(container) || !root[container].is_object() || !root[container].contains(entry)) continue;
			if (agent != nullptr) { out.error = "agent." + entry + ": defined in both agent and agents."; return false; }
			agent = &root[container][entry];
		}
		if (agent == nullptr) { out.error = "agent." + entry + ": not found in this config."; return false; }
		if (!agent->is_object()) { out.error = "agent." + entry + ": expected an object."; return false; }
		static const std::set<std::string> interpreted{"description", "mode", "tools", "permission", "name"};
		for (const auto& [key, value] : agent->items())
		{
			const std::string field_path = "agent." + entry + "." + key;
			if (!SimpleFieldName(key)) { out.error = field_path + ": unsupported field name."; return false; }
			if (key == "prompt")
			{
				if (!value.is_string()) { out.error = "prompt: expected text or a {file:...} reference."; return false; }
				const std::string prompt = value.get<std::string>();
				const std::string_view trimmed = TrimView(prompt);
				if (trimmed.starts_with("{file:") && trimmed.ends_with('}') && trimmed.find('{', 1) == std::string_view::npos)
				{
					const std::string reference(TrimView(trimmed.substr(6, trimmed.size() - 7)));
					std::filesystem::path prompt_path = reference.starts_with("~")
					                                        ? PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(reference)
					                                        : uam::paths::PathFromUtf8(reference);
					if (prompt_path.is_relative()) prompt_path = config_path.parent_path() / prompt_path;
					prompt_path = uam::paths::LexicallyNormalPath(uam::paths::AbsolutePathNoThrow(prompt_path));
					// A config from a cloned project must not pull an unrelated private file into the agent's instructions.
					const std::filesystem::path config_folder = uam::paths::LexicallyNormalPath(uam::paths::AbsolutePathNoThrow(config_path.parent_path()));
					const std::filesystem::path inside = prompt_path.lexically_relative(config_folder);
					if (reference.empty() || inside.empty() || inside.is_absolute() || *inside.begin() == "..")
					{
						out.error = "prompt: the {file:...} reference must name a file inside the config's folder.";
						return false;
					}
					std::string body;
					if (!ReadBoundedFile(prompt_path, kMaxInstructionsBytes, body))
					{
						out.error = "prompt: the referenced file must be a regular, non-linked text file no larger than 192 KiB.";
						return false;
					}
					out.instructions = uam::strings::Trim(body);
					out.prompt_file = uam::paths::PortablePathString(inside);
				}
				else if (prompt.find("{file:") != std::string::npos || prompt.find("{env:") != std::string::npos)
				{
					out.error = "prompt: only a whole {file:...} reference is supported.";
					return false;
				}
				else out.instructions = uam::strings::Trim(prompt);
				continue;
			}
			Node node;
			if (!interpreted.contains(key)) node.kind = Node::Kind::Opaque;
			else if (!JsonToNode(value, key, 1, node, out.error)) return false;
			out.fields.emplace_back(key, std::move(node));
		}
		return true;
	}

	namespace
	{
		bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right)
		{
			return uam::paths::LexicallyNormalPath(uam::paths::AbsolutePathNoThrow(left)) == uam::paths::LexicallyNormalPath(uam::paths::AbsolutePathNoThrow(right));
		}

		/// Config files are the user's own and only add restrictions, so links are followed (dotfile managers link them);
		/// the target must still be a bounded regular file. An empty file is an empty config.
		bool ReadConfigFile(const std::filesystem::path& path, std::string& text)
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::status(path, error);
			if (error || !std::filesystem::is_regular_file(status)) return false;
			const std::uintmax_t size = std::filesystem::file_size(path, error);
			if (error || size > kMaxSourceBytes) return false;
			if (size == 0) { text = "{}"; return true; }
			return uam::io::TryReadTextFile(path, text) && text.size() == size && !HasDisallowedControl(text);
		}

		bool Denies(const OrderedJson& value)
		{
			if (value.is_string()) return Lower(value.get<std::string>()) == "deny";
			if (value.is_boolean()) return !value.get<bool>();
			if (value.is_object()) return std::ranges::any_of(value.items(), [](const auto& entry) { return Denies(entry.value()); });
			return false;
		}

		/// Merge one config's rules into the rules so far. Config order across files is not relied on: a deny already
		/// present is kept, so the merged rules are at least as strict as OpenCode's own merge of the same files.
		void MergeStrict(OrderedJson& merged, const OrderedJson& next)
		{
			if (merged.is_null()) { merged = next; return; }
			if (merged.is_object() && next.is_object())
			{
				for (const auto& [key, value] : next.items())
				{
					if (!merged.contains(key)) merged[key] = value;
					else MergeStrict(merged[key], value);
				}
				return;
			}
			if (Denies(merged) && !Denies(next)) return;
			merged = next;
		}
	} // namespace

	bool AddInheritedConfigRules(const std::filesystem::path& source_path, ParsedSource& out)
	{
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		std::vector<std::filesystem::path> configs;
		const auto add = [&](const std::filesystem::path& path)
		{
			if (!path.empty() && std::ranges::none_of(configs, [&](const auto& existing) { return SamePath(existing, path); })) configs.push_back(path);
		};
		const std::filesystem::path global = OpenCodeConfigRoot();
		if (!global.empty())
			for (const char* name : {"config.json", "opencode.json", "opencode.jsonc"}) add(global / name);
		// A project agent lives in <project>/.opencode/agent(s); a JSON entry lives in <project>/opencode.json(c) or
		// <project>/.opencode/opencode.json(c). Every project config applies, whichever file holds the agent.
		std::filesystem::path directory = source_path.parent_path();
		const std::string extension = Lower(uam::paths::Utf8PathString(source_path.extension()));
		const bool json_source = extension == ".json" || extension == ".jsonc";
		if (!json_source)
		{
			const std::string name = Lower(uam::paths::Utf8PathString(directory.filename()));
			if (name == "agent" || name == "agents") directory = directory.parent_path();
		}
		std::filesystem::path project;
		if (Lower(uam::paths::Utf8PathString(directory.filename())) == ".opencode") project = directory.parent_path();
		else if (json_source) project = directory;
		if (!project.empty() && (global.empty() || !SamePath(project, global)))
			for (const std::filesystem::path& relative : {std::filesystem::path("opencode.jsonc"), std::filesystem::path("opencode.json"),
			                                             std::filesystem::path(".opencode") / "opencode.jsonc", std::filesystem::path(".opencode") / "opencode.json"})
				add(project / relative);
		if (json_source) add(source_path);

		OrderedJson tools;
		OrderedJson permission;
		std::vector<std::string> applied;
		for (const std::filesystem::path& config : configs)
		{
			if (!uam::paths::PathExistsNoThrow(config)) continue;
			const std::string label = uam::paths::Utf8PathString(config.filename());
			std::string text;
			if (!ReadConfigFile(config, text))
			{
				out.error = "config: " + label + " could not be read safely, so its rules cannot be applied.";
				return false;
			}
			std::string error;
			const OrderedJson root = ParseJsonc(text, error);
			if (root.is_discarded() || !root.is_object())
			{
				out.error = "config: " + label + " is not valid JSON or JSONC, so its rules cannot be applied.";
				return false;
			}
			if (root.contains("tools")) MergeStrict(tools, root["tools"]);
			if (root.contains("permission")) MergeStrict(permission, root["permission"]);
			if (root.contains("tools") || root.contains("permission")) applied.push_back(label);
		}
		for (const auto& [field, value] : {std::pair<const char*, const OrderedJson*>{"tools", &tools}, {"permission", &permission}})
		{
			if (value->is_null()) continue;
			InheritedRules rules{"config", field, {}, Join(applied, ", ")};
			if (!JsonToNode(*value, std::string("config ") + field, 1, rules.node, out.error)) return false;
			out.inherited.push_back(std::move(rules));
		}
#else
		(void)source_path;
		(void)out;
#endif
		return true;
	}

	std::vector<std::pair<std::string, std::string>> ListJsonConfigAgents(const std::string& text, std::string& error)
	{
		std::vector<std::pair<std::string, std::string>> agents;
		const OrderedJson root = ParseJsonc(text, error);
		if (root.is_discarded() || !root.is_object()) { if (error.empty()) error = "config: expected a JSON object."; return agents; }
		for (const char* container : {"agent", "agents"})
		{
			if (!root.contains(container) || !root[container].is_object()) continue;
			for (const auto& [name, agent] : root[container].items())
			{
				if (agents.size() >= kMaxFilesPerFolder) return agents;
				std::string description;
				if (agent.is_object() && agent.contains("description") && agent["description"].is_string()) description = agent["description"].get<std::string>();
				agents.emplace_back(name, description);
			}
		}
		return agents;
	}

	std::string SuggestId(std::string_view raw)
	{
		std::string id;
		for (const unsigned char ch : raw)
		{
			const unsigned char lower = static_cast<unsigned char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
			if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) id.push_back(static_cast<char>(lower));
			else if (!id.empty() && id.back() != '-') id.push_back('-');
		}
		if (id.size() > 64) id.resize(64);
		while (!id.empty() && id.back() == '-') id.pop_back();
		return id;
	}

	std::string NormalizeDescription(std::string_view raw, std::size_t limit, std::vector<std::string>* notes)
	{
		std::string text;
		bool space = false;
		bool quotes = false;
		for (const char ch : raw)
		{
			if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { space = true; continue; }
			if (space && !text.empty()) text.push_back(' ');
			space = false;
			if (ch == '"') { quotes = true; text.push_back('\''); continue; }
			text.push_back(ch);
		}
		if (quotes && notes != nullptr) AddNote(*notes, "description: double quotes became single quotes.");
		if (text.size() > limit)
		{
			const std::size_t original = text.size();
			const std::string_view ellipsis = "\xE2\x80\xA6";
			std::size_t cut = limit - ellipsis.size();
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
			text.resize(cut);
			while (!text.empty() && text.back() == ' ') text.pop_back();
			text += ellipsis;
			if (notes != nullptr)
				AddNote(*notes, "description: shortened from " + std::to_string(original) + " to " + std::to_string(text.size()) + " bytes; the full text stays in the source.");
		}
		return text;
	}

	ProviderAgentImportPreview BuildPreview(const std::string& provider_id, ParsedSource source,
	                                        const std::string& file_id, const std::string& local_opencode_version)
	{
		ProviderAgentImportPreview preview;
		preview.provider_id = provider_id;
		const bool opencode = provider_id == provider_ids::kOpenCodeCli;
		const auto fail = [&](std::string message)
		{
			preview.error = std::move(message);
			preview.supported = false;
			return preview;
		};

		preview.instructions = std::move(source.instructions);
		if (!source.prompt_file.empty()) AddNote(preview.notes, "prompt: instructions are read from " + source.prompt_file + ".");
		if (preview.instructions.empty()) return fail("instructions: the agent has no instruction text.");
		if (preview.instructions.size() > kMaxInstructionsBytes) return fail("instructions: larger than 192 KiB.");

		// Identity.
		std::string raw_id = file_id;
		if (const Node* name = source.Find("name"); name != nullptr && name->kind != Node::Kind::Opaque &&
		    (provider_id == provider_ids::kGeminiCli || provider_id == provider_ids::kClaudeCli))
		{
			if (name->kind != Node::Kind::Scalar) return fail("name: expected text.");
			if (!TrimView(name->text).empty()) raw_id = name->text;
		}
		preview.suggested_id = SuggestId(raw_id);
		if (preview.suggested_id.empty()) AddNote(preview.notes, "name: no usable ID in the source; enter a UAM agent ID.");
		else if (preview.suggested_id == "build" || preview.suggested_id == "plan")
		{
			preview.suggested_id = ProviderPrefix(provider_id) + "-" + preview.suggested_id;
			AddNote(preview.notes, "name: Build and Plan are UAM built-ins, so the suggested ID is " + preview.suggested_id + ".");
		}

		// Description.
		const Node* description = source.Find("description");
		if (description != nullptr && description->kind != Node::Kind::Scalar && description->kind != Node::Kind::Null)
			return fail("description: expected text.");
		if (description == nullptr || description->kind == Node::Kind::Null || TrimView(description->text).empty())
		{
			preview.description = "Imported " + ProviderLabel(provider_id) + " agent " + (preview.suggested_id.empty() ? raw_id : preview.suggested_id);
			preview.description = NormalizeDescription(preview.description, kMaxImportedDescriptionBytes, nullptr);
			AddNote(preview.notes, "description: none in the source, so UAM wrote a placeholder.");
		}
		else preview.description = NormalizeDescription(description->text, kMaxImportedDescriptionBytes, &preview.notes);

		// Mode.
		if (opencode)
		{
			const Node* mode = source.Find("mode");
			if (mode == nullptr) preview.mode = "both"; // OpenCode's default is `all`; UAM calls it `both`.
			else if (mode->kind != Node::Kind::Scalar) return fail("mode: expected primary, subagent or all.");
			else
			{
				preview.mode = Lower(mode->text);
				if (preview.mode == "all") preview.mode = "both";
				if (preview.mode != "primary" && preview.mode != "subagent" && preview.mode != "both") return fail("mode: expected primary, subagent or all.");
			}
		}
		else preview.mode = "subagent";

		// Field classification. Blocked fields would change what the agent may do if they were dropped.
		static const std::map<std::string, std::string> blocked{
		    {"hooks", "hooks run provider code that UAM cannot run"},
		    {"isolation", "isolation needs a separate worktree that UAM does not create"}};
		static const std::set<std::string> limits{"max_turns", "maxTurns", "steps", "maxSteps", "timeout_mins"};
		static const std::set<std::string> capabilities{"mcpServers", "mcp-servers", "skills", "memory", "background"};
		static const std::set<std::string> tool_fields{"tools", "allowed-tools", "disallowedTools", "disallowed-tools"};
		static const std::set<std::string> permission_modes{"permissionMode", "permission-mode"};
		const std::set<std::string> mapped = opencode ? std::set<std::string>{"description", "mode", "tools", "permission"}
		                                   : provider_id == provider_ids::kGeminiCli ? std::set<std::string>{"name", "description", "kind"}
		                                                                              : std::set<std::string>{"name", "description"};
		std::vector<std::string> blocked_reasons;
		for (const auto& [key, node] : source.fields)
		{
			(void)node;
			if (const auto found = blocked.find(key); found != blocked.end())
			{
				preview.security_fields.push_back(key);
				blocked_reasons.push_back(key + ": " + found->second);
				continue;
			}
			if (mapped.contains(key)) continue;
			preview.ignored_fields.push_back(key);
			if (limits.contains(key)) AddNote(preview.notes, key + ": UAM does not copy turn, step or time limits.");
			else if (capabilities.contains(key)) AddNote(preview.notes, key + ": not copied; the agent uses UAM skills and MCP settings.");
		}
		if (provider_id == provider_ids::kGeminiCli)
		{
			if (const Node* kind = source.Find("kind"); kind != nullptr && (kind->kind != Node::Kind::Scalar || Lower(kind->text) != "local"))
			{
				preview.security_fields.push_back("kind");
				blocked_reasons.push_back("kind: only local " UAM_IMPORT_GEMINI_NAME " agents run in UAM");
			}
		}
		std::ranges::sort(preview.security_fields);
		std::ranges::sort(preview.ignored_fields);
		if (!blocked_reasons.empty()) return fail(Join(blocked_reasons, "; ") + ". Remove " + (blocked_reasons.size() == 1 ? "it" : "them") + " from the source to import.");

		// Restrictions and the workspace-access ceiling.
		if (opencode)
		{
			NormalizedRules rules;
			if (!NormalizeToolRules(source, preview, rules)) return fail(preview.error);
			std::vector<std::string> bound_denies;
			for (const auto& [key, value] : rules.canonical.items())
				if (value == "deny" && !IsEditTool(key)) bound_denies.push_back(key);
			preview.provider_bound = !bound_denies.empty();
			if (!rules.canonical.empty()) preview.open_code_permission_json = rules.canonical.dump();
			preview.suggested_workspace_access = rules.explicit_write ? "write" : "read";
			if (preview.provider_bound)
			{
				AddNote(preview.notes, "Deny rules (" + Join(bound_denies, ", ") + ") keep this agent to local " UAM_IMPORT_OPENCODE_NAME " 1.x chats without Central Setup; other routes refuse it.");
				if (MajorVersion(local_opencode_version) >= 2)
					return fail("permission: " UAM_IMPORT_OPENCODE_NAME " " + std::string(TrimView(local_opencode_version)) + " cannot run imported agents with deny rules yet, so this import is refused.");
			}
			else if (!rules.canonical.empty())
				AddNote(preview.notes, "permission: workspace access covers every rule, so the agent is not tied to " UAM_IMPORT_OPENCODE_NAME ".");
		}
		else
		{
			const ToolVocabulary vocabulary = VocabularyFor(provider_id);
			std::string error;
			bool explicit_write = false;
			for (const std::string field : {"tools", "allowed-tools"})
			{
				const Node* node = source.Find(field);
				if (node == nullptr || node->kind == Node::Kind::Null) continue; // An empty value means "not specified".
				std::vector<std::string> names;
				if (!ToolNames(node, field, names, error)) return fail(error);
				// Only a listed tool known to edit (or every tool) makes the source writable; unknown tools are assumed not to.
				const bool read_only = std::ranges::none_of(names, [&](const std::string& name) { return vocabulary.write.contains(name) || name == "*" || name == "all"; });
				explicit_write = explicit_write || std::ranges::any_of(names, [&](const std::string& name) { return vocabulary.write.contains(name) || name == "*" || name == "all"; });
				if (read_only && preview.read_only_reason.empty())
				{
					preview.max_workspace_access = "read";
					preview.read_only_reason = field + " lists only read tools";
				}
				AddNote(preview.notes, field + ": UAM keeps the workspace-access limit only; the individual tool list is not copied.");
			}
			for (const std::string field : {"disallowedTools", "disallowed-tools"})
			{
				const Node* node = source.Find(field);
				if (node == nullptr || node->kind == Node::Kind::Null) continue;
				std::vector<std::string> names;
				if (!ToolNames(node, field, names, error)) return fail(error);
				const bool blocks_writes = std::ranges::all_of(vocabulary.write, [&](const std::string& name) { return std::ranges::find(names, name) != names.end(); });
				if (blocks_writes && preview.read_only_reason.empty())
				{
					preview.max_workspace_access = "read";
					preview.read_only_reason = field + " blocks every edit tool";
				}
				AddNote(preview.notes, field + ": UAM keeps the workspace-access limit only; other blocked tools are not copied.");
			}
			preview.suggested_workspace_access = preview.max_workspace_access == "write" && explicit_write ? "write" : "read";
		}
		// A plan permission mode is a read-only ceiling in any provider's file.
		for (const std::string field : {"permissionMode", "permission-mode"})
		{
			const Node* node = source.Find(field);
			if (node == nullptr || node->kind == Node::Kind::Null) continue;
			if (node->kind == Node::Kind::Scalar && Lower(node->text) == "plan")
			{
				if (preview.read_only_reason.empty()) preview.read_only_reason = field + " is plan";
				preview.max_workspace_access = "read";
				preview.suggested_workspace_access = "read";
				AddNote(preview.notes, field + ": plan keeps the agent read-only.");
			}
			else
			{
				if (node->kind == Node::Kind::Scalar && Lower(node->text) == "acceptedits" && preview.max_workspace_access == "write") preview.suggested_workspace_access = "write";
				AddNote(preview.notes, field + ": not copied; the chat's UAM approval settings apply.");
			}
		}
		if (preview.max_workspace_access == "read")
			AddNote(preview.notes, "Workspace access: read only, because " + preview.read_only_reason + ".");
		preview.supported = true;
		return preview;
	}

	std::string BoundPolicyForAccess(const ProviderAgentImportPreview& preview, const std::string& workspace_access)
	{
		if (!preview.provider_bound || preview.open_code_permission_json.empty()) return {};
		OrderedJson rules = OrderedJson::parse(preview.open_code_permission_json, nullptr, false);
		if (!rules.is_object()) return {};
		// Exact keys come last in canonical order, so an edit deny set or appended here decides edit.
		if (workspace_access == "read")
			for (const std::string_view tool : kEditTools)
				if (tool == "edit" || rules.contains(std::string(tool))) rules[std::string(tool)] = "deny";
		return rules.dump();
	}

	bool ValidStoredPolicy(const std::string& policy_json, const std::string& workspace_access)
	{
		std::string error;
		const OrderedJson policy = ParseJsonc(policy_json, error);
		if (policy.is_discarded() || !policy.is_object() || policy.empty() || policy.size() > kMaxRuleEntries) return false;
		for (const auto& [key, value] : policy.items())
			if (!ValidRuleKey(key) || !value.is_string() || (value != "ask" && value != "deny")) return false;
		// Read the rules as OpenCode does: in order, last match wins.
		if (workspace_access == "read") return StoredEffect(policy, "edit") == "deny";
		if (workspace_access != "write") return false;
		return std::ranges::none_of(kEditTools, [&](std::string_view tool) {
			const bool named = tool == "edit" || std::ranges::any_of(policy.items(), [&](const auto& entry) { return entry.key() != "*" && Covers(entry.key(), tool); });
			return named && StoredEffect(policy, tool) == "deny";
		});
	}

	std::string TightenPolicyForRead(const std::string& policy_json)
	{
		OrderedJson policy = OrderedJson::parse(policy_json, nullptr, false);
		if (!policy.is_object()) return {};
		if (ValidStoredPolicy(policy_json, "read")) return policy.dump();
		// Move edit tools to the end so no earlier wildcard can reopen them.
		for (const std::string_view tool : kEditTools)
		{
			const std::string key(tool);
			if (key != "edit" && !policy.contains(key)) continue;
			policy.erase(key);
			policy[key] = "deny";
		}
		return policy.dump();
	}

	bool PoliciesEquivalent(const std::string& left, const std::string& right)
	{
		if (left.empty() || right.empty()) return left.empty() && right.empty();
		const OrderedJson a = OrderedJson::parse(left, nullptr, false);
		const OrderedJson b = OrderedJson::parse(right, nullptr, false);
		if (!a.is_object() || !b.is_object() || a.size() != b.size()) return false;
		// Same rules, and every named tool reads the same under both orders.
		for (const auto& [key, value] : a.items())
			if (!b.contains(key) || b[key] != value || StoredEffect(a, key) != StoredEffect(b, key)) return false;
		return std::ranges::all_of(kEditTools, [&](std::string_view tool) { return StoredEffect(a, tool) == StoredEffect(b, tool); });
	}

	namespace
	{
		struct Folder
		{
			std::filesystem::path path;
			std::string scope;
			std::filesystem::path base; // links are refused from here down
		};

		constexpr std::size_t kMaxEntriesScanned = 4096;

		/// True when `path` or any folder between `base` and it is a link or reparse point.
		bool LinkedBelow(const std::filesystem::path& base, const std::filesystem::path& path)
		{
			// Lexical, so a linked folder is seen rather than resolved through.
			const std::filesystem::path relative = path.lexically_normal().lexically_relative(base.lexically_normal());
			if (relative == ".") return false; // The trusted base itself.
			if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") return uam::paths::IsLinkOrReparsePointNoThrow(path);
			std::filesystem::path current = base;
			for (const std::filesystem::path& component : relative)
			{
				if (component == "." || component == "..") return true;
				current /= component;
				if (uam::paths::IsLinkOrReparsePointNoThrow(current)) return true;
			}
			return false;
		}

		std::string MarkdownFileId(const std::filesystem::path& path)
		{
			std::string filename = Lower(uam::paths::Utf8PathString(path.filename()));
			if (filename.ends_with(".agent.md")) filename.resize(filename.size() - 9);
			else if (filename.ends_with(".md")) filename.resize(filename.size() - 3);
			return filename;
		}

		bool AlreadyImported(const std::string& id, const std::filesystem::path& data_root, const std::filesystem::path& workspace_root)
		{
			if (id.empty()) return false;
			return (!data_root.empty() && uam::paths::PathExistsNoThrow(data_root / "agents" / (id + ".md"))) ||
			       (!workspace_root.empty() && uam::paths::PathExistsNoThrow(workspace_root / ".uam" / "agents" / (id + ".md")));
		}
	} // namespace

	ProviderAgentDiscovery Discover(const std::string& provider_id, const std::filesystem::path& data_root,
	                                const std::filesystem::path& workspace_root)
	{
		ProviderAgentDiscovery discovery;
		const bool opencode = provider_id == provider_ids::kOpenCodeCli;
		if (!opencode && provider_id != provider_ids::kClaudeCli && provider_id != provider_ids::kGeminiCli && provider_id != provider_ids::kCopilotCli)
		{
			discovery.unsupported = "This provider has no native agent files to import.";
			return discovery;
		}
		const std::optional<std::filesystem::path> home = PlatformServicesFactory::Instance().path_service.ResolveUserHomePath();
		std::vector<Folder> folders;
		std::vector<Folder> configs;
		const auto global = [&](const std::filesystem::path& relative) { if (home) folders.push_back({*home / relative, "global", *home}); };
		const auto workspace = [&](const std::filesystem::path& relative) { if (!workspace_root.empty()) folders.push_back({workspace_root / relative, "workspace", workspace_root}); };
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		if (opencode)
		{
			const std::filesystem::path config_root = OpenCodeConfigRoot();
			if (!config_root.empty())
			{
				const std::filesystem::path base = config_root.parent_path();
				folders.push_back({config_root / "agent", "global", base});
				folders.push_back({config_root / "agents", "global", base});
				for (const char* name : {"opencode.json", "opencode.jsonc", "config.json"}) configs.push_back({config_root / name, "global", base});
			}
			workspace(std::filesystem::path(".opencode") / "agent");
			workspace(std::filesystem::path(".opencode") / "agents");
			if (!workspace_root.empty())
				for (const std::filesystem::path& relative : {std::filesystem::path("opencode.json"), std::filesystem::path("opencode.jsonc"),
				                                             std::filesystem::path(".opencode") / "opencode.json", std::filesystem::path(".opencode") / "opencode.jsonc"})
					configs.push_back({workspace_root / relative, "workspace", workspace_root});
		}
#endif
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI
		if (provider_id == provider_ids::kClaudeCli) { global(std::filesystem::path(".claude") / "agents"); workspace(std::filesystem::path(".claude") / "agents"); }
#endif
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
		if (provider_id == provider_ids::kGeminiCli) { global(std::filesystem::path(".gemini") / "agents"); workspace(std::filesystem::path(".gemini") / "agents"); }
#endif
#if UAM_ENABLE_RUNTIME_COPILOT_CLI
		if (provider_id == provider_ids::kCopilotCli) { global(std::filesystem::path(".copilot") / "agents"); workspace(std::filesystem::path(".github") / "agents"); }
#endif

		std::uintmax_t bytes = 0;
		static const std::set<std::string> summary_fields{"name", "description"};
		for (const Folder& folder : folders)
		{
			discovery.searched.push_back(folder.path);
			if (!uam::paths::PathExistsNoThrow(folder.path)) continue;
			if (LinkedBelow(folder.base, folder.path) || !uam::paths::IsDirectoryNoThrow(folder.path))
			{
				discovery.errors.push_back(uam::paths::Utf8PathString(folder.path) + ": linked or non-folder agent locations are not searched.");
				continue;
			}
			std::vector<std::filesystem::path> files;
			std::error_code error;
			std::size_t scanned = 0;
			for (std::filesystem::directory_iterator it(folder.path, error), end; !error && it != end; it.increment(error))
			{
				if (++scanned > kMaxEntriesScanned) { discovery.errors.push_back(uam::paths::Utf8PathString(folder.path) + ": only the first 4096 entries are searched."); break; }
				if (Lower(uam::paths::Utf8PathString(it->path().extension())) != ".md") continue;
				if (files.size() == kMaxFilesPerFolder) { discovery.errors.push_back(uam::paths::Utf8PathString(folder.path) + ": only the first 256 agent files are listed."); break; }
				files.push_back(it->path());
			}
			if (error) { discovery.errors.push_back(uam::paths::Utf8PathString(folder.path) + ": the folder could not be listed."); continue; }
			std::ranges::sort(files);
			for (const std::filesystem::path& file : files)
			{
				std::string text;
				const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(file);
				if (!size || bytes + *size > kMaxDiscoveryBytes || !ReadBoundedFile(file, kMaxSourceBytes, text))
				{
					discovery.errors.push_back(uam::paths::Utf8PathString(file) + ": skipped because it is linked, empty, unreadable or larger than 256 KiB.");
					continue;
				}
				bytes += *size;
				ParsedSource parsed;
				(void)ParseMarkdownAgent(text, summary_fields, parsed); // A parse error still lists the file; preview names the problem.
				ProviderAgentCandidate candidate;
				candidate.provider_id = provider_id;
				std::string raw_id = MarkdownFileId(file);
				if (provider_id == provider_ids::kGeminiCli || provider_id == provider_ids::kClaudeCli)
					if (const Node* name = parsed.Find("name"); name != nullptr && name->kind == Node::Kind::Scalar && !name->text.empty()) raw_id = name->text;
				candidate.id = SuggestId(raw_id);
				if (const Node* description = parsed.Find("description"); description != nullptr && description->kind == Node::Kind::Scalar)
					candidate.description = NormalizeDescription(description->text, 240, nullptr);
				candidate.source_path = file;
				candidate.scope = folder.scope;
				candidate.already_imported = AlreadyImported(candidate.id, data_root, workspace_root);
				discovery.candidates.push_back(std::move(candidate));
			}
		}
		for (const Folder& config : configs)
		{
			discovery.searched.push_back(config.path);
			if (!uam::paths::PathExistsNoThrow(config.path)) continue;
			if (LinkedBelow(config.base, config.path.parent_path()))
			{
				discovery.errors.push_back(uam::paths::Utf8PathString(config.path) + ": linked agent locations are not searched.");
				continue;
			}
			std::string text;
			const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(config.path);
			if (!size || bytes + *size > kMaxDiscoveryBytes || !ReadBoundedFile(config.path, kMaxSourceBytes, text))
			{
				discovery.errors.push_back(uam::paths::Utf8PathString(config.path) + ": skipped because it is linked, empty, unreadable or larger than 256 KiB.");
				continue;
			}
			bytes += *size;
			std::string error;
			const auto agents = ListJsonConfigAgents(text, error);
			if (!error.empty()) { discovery.errors.push_back(uam::paths::Utf8PathString(config.path) + ": " + error); continue; }
			for (const auto& [name, description] : agents)
			{
				ProviderAgentCandidate candidate;
				candidate.provider_id = provider_id;
				candidate.id = SuggestId(name);
				candidate.description = NormalizeDescription(description, 240, nullptr);
				candidate.source_path = config.path;
				candidate.source_entry = name;
				candidate.scope = config.scope;
				candidate.already_imported = AlreadyImported(candidate.id, data_root, workspace_root);
				discovery.candidates.push_back(std::move(candidate));
			}
		}
		return discovery;
	}
} // namespace uam::provider_agent_import
