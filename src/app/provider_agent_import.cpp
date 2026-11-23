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
		constexpr std::array<std::string_view, 5> kOpenCodeWriteTools{"edit", "write", "patch", "multiedit", "apply_patch"};

		bool IsOpenCodeWriteTool(std::string_view key)
		{
			return std::ranges::find(kOpenCodeWriteTools, key) != kOpenCodeWriteTools.end();
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

		std::string ProviderLabel(const std::string& provider_id)
		{
			if (provider_id == provider_ids::kOpenCodeCli) return "OpenCode";
			if (provider_id == provider_ids::kCopilotCli) return "Copilot";
			if (provider_id == provider_ids::kGeminiCli) return "Gemini";
			if (provider_id == provider_ids::kClaudeCli) return "Claude";
			return "provider";
		}

		std::string ProviderPrefix(const std::string& provider_id)
		{
			if (provider_id == provider_ids::kOpenCodeCli) return "opencode";
			if (provider_id == provider_ids::kCopilotCli) return "copilot";
			if (provider_id == provider_ids::kGeminiCli) return "gemini";
			return "claude";
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

		struct RuleSet
		{
			nlohmann::json rules = nlohmann::json::object();
			std::map<std::string, std::string> deny_origin;
			bool explicit_write = false;

			void Put(const std::string& key, const std::string& effect, const std::string& origin)
			{
				if (!rules.contains(key) || effect == "deny") rules[key] = effect;
				if (effect == "deny" && !deny_origin.contains(key)) deny_origin[key] = origin;
			}

			std::string Effective(const std::string& key) const
			{
				if (rules.contains(key)) return rules[key].get<std::string>();
				if (rules.contains("*")) return rules["*"].get<std::string>();
				return "ask";
			}
		};

		bool NormalizeOpenCodeRules(const ParsedSource& source, ProviderAgentImportPreview& preview, RuleSet& set)
		{
			if (const Node* tools = source.Find("tools"); tools != nullptr && tools->kind != Node::Kind::Null)
			{
				if (tools->kind != Node::Kind::Map) { preview.error = "tools: expected a map of tool: true or false."; return false; }
				for (const auto& [key, value] : tools->entries)
				{
					const std::string path = "tools." + key;
					if (!ValidRuleKey(key)) { preview.error = path + ": unsupported tool name."; return false; }
					const std::string flag = value.kind == Node::Kind::Scalar ? Lower(value.text) : std::string{};
					if (flag != "true" && flag != "false") { preview.error = path + ": expected true or false."; return false; }
					set.Put(key, flag == "true" ? "ask" : "deny", path);
					if (flag == "true" && IsOpenCodeWriteTool(key)) set.explicit_write = true;
				}
				AddNote(preview.notes, "tools: enabled tools become ask and disabled tools become deny.");
			}
			if (const Node* permission = source.Find("permission"); permission != nullptr && permission->kind != Node::Kind::Null)
			{
				if (permission->kind == Node::Kind::Scalar)
				{
					const std::optional<std::string> effect = RuleEffect(*permission);
					if (!effect) { preview.error = "permission: expected allow, ask or deny."; return false; }
					set.Put("*", *effect, "permission");
					if (Lower(permission->text) == "allow") set.explicit_write = true;
				}
				else if (permission->kind == Node::Kind::Map)
				{
					for (const auto& [key, value] : permission->entries)
					{
						const std::string path = "permission." + key;
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
							set.Put(key, any_deny ? "deny" : "ask", path);
							if (!any_deny && IsOpenCodeWriteTool(key)) set.explicit_write = true;
							continue;
						}
						const std::optional<std::string> effect = RuleEffect(value);
						if (!effect) { preview.error = path + ": expected allow, ask or deny."; return false; }
						set.Put(key, *effect, path);
						if (*effect == "ask" && IsOpenCodeWriteTool(key)) set.explicit_write = true;
						if (Lower(value.text) == "allow") AddNote(preview.notes, "permission: allow becomes ask, so UAM still asks before each use.");
					}
				}
				else { preview.error = "permission: expected allow, ask, deny or a map of tool rules."; return false; }
			}
			if (set.rules.size() > kMaxRuleEntries) { preview.error = "permission: more than 64 tool rules."; return false; }

			// OpenCode can check several edit tools under one rule, so one denied edit tool denies them all.
			std::string reason;
			if (set.Effective("edit") == "deny")
				reason = set.deny_origin.contains("edit") ? set.deny_origin.at("edit") : set.deny_origin.contains("*") ? set.deny_origin.at("*") : "permission.edit";
			for (const std::string_view tool : kOpenCodeWriteTools)
			{
				const std::string key(tool);
				if (reason.empty() && set.rules.contains(key) && set.rules[key] == "deny") reason = set.deny_origin.contains(key) ? set.deny_origin.at(key) : key;
			}
			if (!reason.empty())
			{
				bool tightened = false;
				for (const std::string_view tool : kOpenCodeWriteTools)
				{
					const std::string key(tool);
					if (key != "edit" && !set.rules.contains(key)) continue;
					if (set.rules.contains(key) && set.rules[key] != "deny") tightened = true;
					set.rules[key] = "deny";
				}
				if (tightened) AddNote(preview.notes, "edit tools: OpenCode may check file edits under one rule, so every edit tool is denied.");
				preview.max_workspace_access = "read";
				preview.read_only_reason = reason + " is deny";
			}
			return true;
		}
	} // namespace

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

	bool ParseOpenCodeJsonAgent(const std::string& text, const std::string& entry,
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
			const std::string path = "agent." + entry + "." + key;
			if (!SimpleFieldName(key)) { out.error = path + ": unsupported field name."; return false; }
			if (key == "prompt")
			{
				if (!value.is_string()) { out.error = "prompt: expected text or a {file:...} reference."; return false; }
				const std::string prompt = value.get<std::string>();
				const std::string_view trimmed = TrimView(prompt);
				if (trimmed.starts_with("{file:") && trimmed.ends_with('}') && trimmed.find('{', 1) == std::string_view::npos)
				{
					const std::string reference(TrimView(trimmed.substr(6, trimmed.size() - 7)));
					std::filesystem::path path = reference.starts_with("~")
					                                 ? PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(reference)
					                                 : uam::paths::PathFromUtf8(reference);
					if (path.is_relative()) path = config_path.parent_path() / path;
					std::string body;
					if (reference.empty() || !ReadBoundedFile(path, kMaxInstructionsBytes, body))
					{
						out.error = "prompt: the referenced file must be a regular, non-linked text file no larger than 192 KiB.";
						return false;
					}
					out.instructions = uam::strings::Trim(body);
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

	std::vector<std::pair<std::string, std::string>> ListOpenCodeJsonAgents(const std::string& text, std::string& error)
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
				blocked_reasons.push_back("kind: only local Gemini agents run in UAM");
			}
		}
		std::ranges::sort(preview.security_fields);
		std::ranges::sort(preview.ignored_fields);
		if (!blocked_reasons.empty()) return fail(Join(blocked_reasons, "; ") + ". Remove " + (blocked_reasons.size() == 1 ? "it" : "them") + " from the source to import.");

		// Restrictions and the workspace-access ceiling.
		if (opencode)
		{
			RuleSet rules;
			if (!NormalizeOpenCodeRules(source, preview, rules)) return fail(preview.error);
			std::vector<std::string> bound_denies;
			for (const auto& [key, value] : rules.rules.items())
				if (value == "deny" && !IsOpenCodeWriteTool(key)) bound_denies.push_back(key);
			preview.provider_bound = !bound_denies.empty();
			if (!rules.rules.empty()) preview.open_code_permission_json = rules.rules.dump();
			preview.suggested_workspace_access = preview.max_workspace_access == "write" && rules.explicit_write ? "write" : "read";
			if (preview.provider_bound)
			{
				AddNote(preview.notes, "Deny rules (" + Join(bound_denies, ", ") + ") keep this agent to local OpenCode 1.x chats without Central Setup; other routes refuse it.");
				if (MajorVersion(local_opencode_version) >= 2)
					return fail("permission: OpenCode " + std::string(TrimView(local_opencode_version)) + " cannot run imported agents with deny rules yet, so this import is refused.");
			}
			else if (!rules.rules.empty())
				AddNote(preview.notes, "permission: workspace access covers every rule, so the agent is not tied to OpenCode.");
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
				const bool read_only = std::ranges::all_of(names, [&](const std::string& name) { return vocabulary.read_only.contains(name); });
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
		nlohmann::json rules = nlohmann::json::parse(preview.open_code_permission_json, nullptr, false);
		if (!rules.is_object()) return {};
		if (workspace_access == "read")
			for (const std::string_view tool : kOpenCodeWriteTools)
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
		const std::string edit = policy.contains("edit") ? policy["edit"].get<std::string>() : policy.contains("*") ? policy["*"].get<std::string>() : "ask";
		if (workspace_access == "read") return edit == "deny";
		if (workspace_access != "write" || edit == "deny") return false;
		return std::ranges::none_of(kOpenCodeWriteTools, [&](std::string_view tool) {
			return policy.contains(std::string(tool)) && policy[std::string(tool)] == "deny";
		});
	}

	std::string TightenPolicyForRead(const std::string& policy_json)
	{
		OrderedJson policy = OrderedJson::parse(policy_json, nullptr, false);
		if (!policy.is_object()) return {};
		for (const std::string_view tool : kOpenCodeWriteTools)
			if (tool == "edit" || policy.contains(std::string(tool))) policy[std::string(tool)] = "deny";
		return policy.dump();
	}

	namespace
	{
		struct Folder
		{
			std::filesystem::path path;
			std::string scope;
		};

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
		const auto global = [&](const std::filesystem::path& relative) { if (home) folders.push_back({*home / relative, "global"}); };
		const auto workspace = [&](const std::filesystem::path& relative) { if (!workspace_root.empty()) folders.push_back({workspace_root / relative, "workspace"}); };
		if (opencode)
		{
			std::filesystem::path config_root;
			if (const auto xdg = uam::env::GetNonEmptyString("XDG_CONFIG_HOME")) config_root = uam::paths::PathFromUtf8(*xdg) / "opencode";
			else if (home) config_root = *home / ".config" / "opencode";
			if (!config_root.empty())
			{
				folders.push_back({config_root / "agent", "global"});
				folders.push_back({config_root / "agents", "global"});
				for (const char* name : {"opencode.json", "opencode.jsonc", "config.json"}) configs.push_back({config_root / name, "global"});
			}
			workspace(std::filesystem::path(".opencode") / "agent");
			workspace(std::filesystem::path(".opencode") / "agents");
			if (!workspace_root.empty())
				for (const std::filesystem::path& relative : {std::filesystem::path("opencode.json"), std::filesystem::path("opencode.jsonc"),
				                                             std::filesystem::path(".opencode") / "opencode.json", std::filesystem::path(".opencode") / "opencode.jsonc"})
					configs.push_back({workspace_root / relative, "workspace"});
		}
		else if (provider_id == provider_ids::kClaudeCli) { global(std::filesystem::path(".claude") / "agents"); workspace(std::filesystem::path(".claude") / "agents"); }
		else if (provider_id == provider_ids::kGeminiCli) { global(std::filesystem::path(".gemini") / "agents"); workspace(std::filesystem::path(".gemini") / "agents"); }
		else { global(std::filesystem::path(".copilot") / "agents"); workspace(std::filesystem::path(".github") / "agents"); }

		std::uintmax_t bytes = 0;
		static const std::set<std::string> summary_fields{"name", "description"};
		for (const Folder& folder : folders)
		{
			discovery.searched.push_back(folder.path);
			if (!uam::paths::PathExistsNoThrow(folder.path)) continue;
			if (uam::paths::IsLinkOrReparsePointNoThrow(folder.path) || !uam::paths::IsDirectoryNoThrow(folder.path))
			{
				discovery.errors.push_back(uam::paths::Utf8PathString(folder.path) + ": linked or non-folder agent locations are not searched.");
				continue;
			}
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (std::filesystem::directory_iterator it(folder.path, error), end; !error && it != end; it.increment(error))
			{
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
			std::string text;
			const std::optional<std::uintmax_t> size = uam::paths::FileSizeNoThrow(config.path);
			if (!size || bytes + *size > kMaxDiscoveryBytes || !ReadBoundedFile(config.path, kMaxSourceBytes, text))
			{
				discovery.errors.push_back(uam::paths::Utf8PathString(config.path) + ": skipped because it is linked, empty, unreadable or larger than 256 KiB.");
				continue;
			}
			bytes += *size;
			std::string error;
			const auto agents = ListOpenCodeJsonAgents(text, error);
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
