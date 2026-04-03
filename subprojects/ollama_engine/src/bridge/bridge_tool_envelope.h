#pragma once

#include "bridge_common.h"
#include "bridge_json.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

namespace bridge
{
	struct ToolDefinition
	{
		std::string name;
		std::string description;
		JsonValue parameters = MakeJsonObject();
	};

	inline std::string ExtractContentText(const JsonValue* content)
	{
		if (content == nullptr)
		{
			return "";
		}

		if (content->type == JsonType::String)
		{
			return content->string_value;
		}

		if (content->type == JsonType::Object)
		{
			const JsonValue* text = content->Find("text");
			return (text != nullptr && text->type == JsonType::String) ? text->string_value : "";
		}

		if (content->type != JsonType::Array)
		{
			return "";
		}

		std::ostringstream out;
		bool first = true;

		for (const JsonValue& item : content->array_value)
		{
			std::string piece;

			if (item.type == JsonType::String)
			{
				piece = item.string_value;
			}
			else if (item.type == JsonType::Object)
			{
				const std::string item_type = ToLowerAscii(JsonStringOrEmpty(item.Find("type")));

				if (item_type.empty() || item_type == "text")
				{
					piece = JsonStringOrEmpty(item.Find("text"));
				}
			}

			piece = Trim(piece);

			if (piece.empty())
			{
				continue;
			}

			if (!first)
			{
				out << "\n";
			}

			out << piece;
			first = false;
		}

		return out.str();
	}

	inline std::vector<ToolDefinition> ParseToolDefinitions(const JsonValue* tools)
	{
		std::vector<ToolDefinition> out;

		if (tools == nullptr || tools->type != JsonType::Array)
		{
			return out;
		}

		out.reserve(tools->array_value.size());

		for (const JsonValue& entry : tools->array_value)
		{
			if (entry.type != JsonType::Object)
			{
				continue;
			}

			const JsonValue* function = entry.Find("function");

			if (function == nullptr || function->type != JsonType::Object)
			{
				continue;
			}

			ToolDefinition tool;
			tool.name = Trim(JsonStringOrEmpty(function->Find("name")));
			tool.description = Trim(JsonStringOrEmpty(function->Find("description")));

			if (tool.name.empty())
			{
				continue;
			}

			if (const JsonValue* parameters = function->Find("parameters"); parameters != nullptr)
			{
				tool.parameters = *parameters;
			}

			out.push_back(std::move(tool));
		}

		return out;
	}

	inline std::string BuildTranscriptPrompt(const JsonValue& messages)
	{
		std::ostringstream out;
		const std::size_t total_messages = messages.array_value.size();
		constexpr std::size_t kMaxMessages = 14;
		const std::size_t start_index = (total_messages > kMaxMessages) ? (total_messages - kMaxMessages) : 0;

		for (std::size_t index = start_index; index < total_messages; ++index)
		{
			const JsonValue& entry = messages.array_value[index];

			if (entry.type != JsonType::Object)
			{
				continue;
			}

			std::string role = Trim(JsonStringOrEmpty(entry.Find("role")));

			if (role.empty())
			{
				role = "user";
			}

			const std::string role_lower = ToLowerAscii(role);

			std::string content = Trim(ExtractContentText(entry.Find("content")));

			if (content.empty())
			{
				const JsonValue* tool_calls = entry.Find("tool_calls");

				if (tool_calls != nullptr && tool_calls->type == JsonType::Array)
				{
					std::ostringstream synthesized;

					for (const JsonValue& call : tool_calls->array_value)
					{
						if (call.type != JsonType::Object)
						{
							continue;
						}

						const JsonValue* function = call.Find("function");

						if (function == nullptr || function->type != JsonType::Object)
						{
							continue;
						}

						const std::string name = Trim(JsonStringOrEmpty(function->Find("name")));
						const std::string arguments = Trim(JsonStringOrEmpty(function->Find("arguments")));

						if (name.empty() && arguments.empty())
						{
							continue;
						}

						synthesized << "tool_call";

						if (!name.empty())
						{
							synthesized << " name=" << name;
						}

						if (!arguments.empty())
						{
							synthesized << " arguments=" << arguments;
						}

						synthesized << "\n";
					}

					content = Trim(synthesized.str());
				}
			}

			if (content.empty())
			{
				continue;
			}

			const std::size_t per_message_limit = (role_lower == "system") ? 420 : 1400;

			if (content.size() > per_message_limit)
			{
				content = content.substr(0, per_message_limit) + "\n...[truncated]";
			}

			out << "[" << role << "] " << content << "\n";
		}

		std::string transcript = Trim(out.str());
		constexpr std::size_t kMaxTranscriptChars = 12000;

		if (transcript.size() > kMaxTranscriptChars)
		{
			const std::size_t cut = transcript.size() - kMaxTranscriptChars;
			const std::size_t next_newline = transcript.find('\n', cut);

			if (next_newline != std::string::npos && next_newline + 1 < transcript.size())
			{
				transcript = transcript.substr(next_newline + 1);
			}
			else
			{
				transcript = transcript.substr(cut);
			}
		}

		return Trim(transcript);
	}

	inline std::string BuildToolEnvelopePrompt(const std::string& transcript, const std::vector<ToolDefinition>& tools)
	{
		std::ostringstream out;
		out << "You are a local tool-routing assistant.\n";
		out << "When tools are available, you MUST respond with strict JSON only.\n";
		out << "Allowed response envelope shapes:\n";
		out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
		out << "{\"type\":\"text\",\"text\":\"<assistant text reply>\"}\n";
		out << "Do not include markdown fences and do not include extra keys.\n";
		out << "\nAvailable tools:\n";

		for (const ToolDefinition& tool : tools)
		{
			out << "- name: " << tool.name << "\n";

			if (!tool.description.empty())
			{
				out << "  description: " << tool.description << "\n";
			}

			out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
		}

		out << "\nConversation transcript:\n";
		out << transcript << "\n";
		out << "\nRespond with a single valid JSON object now.";
		return out.str();
	}

	inline std::string BuildToolEnvelopeRepairPrompt(const std::string& transcript, const std::vector<ToolDefinition>& tools, const std::string& invalid_output)
	{
		std::ostringstream out;
		out << "Your previous response was invalid JSON or incomplete.\n";
		out << "Return exactly ONE valid JSON object with no markdown.\n";
		out << "Allowed shapes only:\n";
		out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
		out << "{\"type\":\"text\",\"text\":\"<assistant text reply>\"}\n";
		out << "\nAvailable tools:\n";

		for (const ToolDefinition& tool : tools)
		{
			out << "- name: " << tool.name << "\n";

			if (!tool.description.empty())
			{
				out << "  description: " << tool.description << "\n";
			}

			out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
		}

		out << "\nConversation transcript:\n";
		out << transcript << "\n";
		out << "\nInvalid previous output:\n";
		out << invalid_output << "\n";
		out << "\nReturn corrected JSON only.";
		return out.str();
	}

	inline std::string LatestUserMessageText(const JsonValue& messages)
	{
		if (messages.type != JsonType::Array)
		{
			return "";
		}

		for (auto it = messages.array_value.rbegin(); it != messages.array_value.rend(); ++it)
		{
			if (it->type != JsonType::Object)
			{
				continue;
			}

			const std::string role = ToLowerAscii(Trim(JsonStringOrEmpty(it->Find("role"))));

			if (role != "user")
			{
				continue;
			}

			const std::string content = Trim(ExtractContentText(it->Find("content")));

			if (!content.empty())
			{
				return content;
			}
		}

		return "";
	}

	inline std::string StripMarkdownCodeFence(const std::string& text)
	{
		const std::string trimmed = Trim(text);

		if (trimmed.size() < 6 || trimmed.rfind("```", 0) != 0)
		{
			return trimmed;
		}

		const std::size_t first_newline = trimmed.find('\n');

		if (first_newline == std::string::npos)
		{
			return trimmed;
		}

		const std::size_t closing = trimmed.rfind("```");

		if (closing == std::string::npos || closing <= first_newline)
		{
			return trimmed;
		}

		return Trim(trimmed.substr(first_newline + 1, closing - first_newline - 1));
	}

	inline bool LooksLikeToolEnvelopeCandidate(const std::string& text)
	{
		const std::string candidate = Trim(StripMarkdownCodeFence(text));
		return !candidate.empty() && candidate.front() == '{';
	}

	inline std::optional<std::string> FindAllowedToolNameIgnoreCase(const std::set<std::string>& allowed_tool_names, const std::string& desired_lower)
	{
		for (const std::string& candidate : allowed_tool_names)
		{
			if (ToLowerAscii(candidate) == desired_lower)
			{
				return candidate;
			}
		}

		return std::nullopt;
	}

	inline std::string BuildHeuristicBashCommand(const std::string& user_request)
	{
		const std::string request = Trim(user_request);
		const std::string lower = ToLowerAscii(request);

		if (request.empty())
		{
			return "pwd";
		}

		if ((lower.find("python") != std::string::npos || lower.find(".py") != std::string::npos) && (lower.find("script") != std::string::npos || lower.find("file") != std::string::npos || lower.find("run") != std::string::npos))
		{
			return "cat > script.py << 'EOF'\nprint('Hello, World!')\nEOF\npython3 script.py";
		}

		if (lower.find("list") != std::string::npos && (lower.find("file") != std::string::npos || lower.find("folder") != std::string::npos || lower.find("directory") != std::string::npos))
		{
			return "ls -la";
		}

		if (lower.find("current directory") != std::string::npos || lower.find("working directory") != std::string::npos || lower == "pwd" || lower.find("where am i") != std::string::npos)
		{
			return "pwd";
		}

		if (lower.find("tree") != std::string::npos)
		{
			return "find . -maxdepth 2 -print";
		}

		if (lower.find("git status") != std::string::npos)
		{
			return "git status --short";
		}

		return "printf '%s\\n' " + ShellSingleQuote("Fallback: unable to derive safe executable command from request: " + request);
	}

	inline std::string NormalizeBashCommandString(const std::string& raw_command)
	{
		std::string command = Trim(StripMarkdownCodeFence(raw_command));

		if (command.rfind("$ ", 0) == 0)
		{
			command = Trim(command.substr(2));
		}

		if (command.rfind("echo ", 0) != 0)
		{
			return command;
		}

		std::string rest = Trim(command.substr(5));

		if (rest.size() < 2)
		{
			return command;
		}

		const char quote = rest.front();

		if (quote != '"' && quote != '\'')
		{
			return command;
		}

		std::size_t close_index = std::string::npos;
		bool escaped = false;

		for (std::size_t i = 1; i < rest.size(); ++i)
		{
			const char ch = rest[i];

			if (quote == '"' && ch == '\\' && !escaped)
			{
				escaped = true;
				continue;
			}

			if (ch == quote && !escaped)
			{
				close_index = i;
				break;
			}

			escaped = false;
		}

		if (close_index == std::string::npos)
		{
			return command;
		}

		std::string payload = rest.substr(1, close_index - 1);
		std::string remainder = Trim(rest.substr(close_index + 1));

		if (remainder.rfind(";", 0) == 0)
		{
			remainder = Trim(remainder.substr(1));
		}

		payload = ReplaceAll(payload, "\\n", "\n");
		payload = ReplaceAll(payload, "\\t", "\t");
		payload = ReplaceAll(payload, "\\\"", "\"");
		payload = ReplaceAll(payload, "\\'", "'");
		payload = ReplaceAll(payload, "\\\\", "\\");

		if (!remainder.empty())
		{
			payload += "\n" + remainder;
		}

		payload = Trim(payload);
		const bool looks_like_script = payload.find('\n') != std::string::npos || payload.find("cat ") != std::string::npos || payload.find("python") != std::string::npos || payload.find("node ") != std::string::npos || payload.find("chmod ") != std::string::npos || payload.find("cd ") != std::string::npos;
		return looks_like_script ? payload : command;
	}

	inline std::string BuildBashRescuePrompt(const std::string& user_request, const std::string& previous_output)
	{
		std::ostringstream out;
		out << "You are preparing arguments for a bash tool call.\n";
		out << "Return ONLY one of the following:\n";
		out << "1) {\"command\":\"<shell command>\"}\n";
		out << "2) Raw shell command text\n";
		out << "Do not include markdown fences and do not include explanations.\n";
		out << "Never wrap commands with echo.\n";
		out << "Prefer a short, direct command sequence that can run as-is.\n";
		out << "\nUser request:\n";
		out << user_request << "\n";

		if (!Trim(previous_output).empty())
		{
			out << "\nPrevious model output (may be invalid):\n";
			out << previous_output << "\n";
		}

		out << "\nReturn bash command now.";
		return out.str();
	}

	inline std::string ExtractBashCommandFromText(const std::string& raw_response)
	{
		const std::string stripped = Trim(StripMarkdownCodeFence(raw_response));

		if (stripped.empty())
		{
			return "";
		}

		const std::optional<JsonValue> parsed = ParseJson(stripped);

		if (parsed.has_value() && parsed->type == JsonType::Object)
		{
			const JsonValue* command = parsed->Find("command");

			if (command != nullptr && command->type == JsonType::String)
			{
				return NormalizeBashCommandString(command->string_value);
			}

			const JsonValue* arguments = parsed->Find("arguments");

			if (arguments != nullptr)
			{
				if (arguments->type == JsonType::Object)
				{
					const JsonValue* nested_command = arguments->Find("command");

					if (nested_command != nullptr && nested_command->type == JsonType::String)
					{
						return NormalizeBashCommandString(nested_command->string_value);
					}
				}
				else if (arguments->type == JsonType::String)
				{
					const std::string nested = Trim(arguments->string_value);

					if (!nested.empty())
					{
						if (const std::optional<JsonValue> nested_json = ParseJson(nested); nested_json.has_value() && nested_json->type == JsonType::Object)
						{
							const JsonValue* nested_command = nested_json->Find("command");

							if (nested_command != nullptr && nested_command->type == JsonType::String)
							{
								return NormalizeBashCommandString(nested_command->string_value);
							}
						}

						return NormalizeBashCommandString(nested);
					}
				}
			}
		}

		if (!stripped.empty() && (stripped.front() == '{' || stripped.front() == '['))
		{
			return "";
		}

		return NormalizeBashCommandString(stripped);
	}

	struct ParsedToolEnvelope
	{
		bool is_tool_call = false;
		std::string tool_name;
		std::string tool_arguments_json;
		std::string text;
	};

	inline std::optional<ParsedToolEnvelope> SynthesizeFallbackToolCall(const std::set<std::string>& allowed_tool_names, const std::string& user_request)
	{
		if (const std::optional<std::string> bash_tool = FindAllowedToolNameIgnoreCase(allowed_tool_names, "bash"); bash_tool.has_value())
		{
			ParsedToolEnvelope out;
			out.is_tool_call = true;
			out.tool_name = bash_tool.value();
			JsonValue args = MakeJsonObject();
			args.object_value["description"] = MakeJsonString("Execute fallback command derived from user request.");
			args.object_value["command"] = MakeJsonString(BuildHeuristicBashCommand(user_request));
			out.tool_arguments_json = SerializeJsonCompact(args);
			return out;
		}

		if (const std::optional<std::string> question_tool = FindAllowedToolNameIgnoreCase(allowed_tool_names, "question"); question_tool.has_value())
		{
			ParsedToolEnvelope out;
			out.is_tool_call = true;
			out.tool_name = question_tool.value();
			JsonValue args = MakeJsonObject();
			args.object_value["question"] = MakeJsonString("Please provide the exact command or concrete file changes to run.");
			out.tool_arguments_json = SerializeJsonCompact(args);
			return out;
		}

		if (!allowed_tool_names.empty())
		{
			ParsedToolEnvelope out;
			out.is_tool_call = true;
			out.tool_name = *allowed_tool_names.begin();
			out.tool_arguments_json = "{}";
			return out;
		}

		return std::nullopt;
	}

	inline std::string BuildToolCallOnlyRepairPrompt(const std::string& transcript, const std::vector<ToolDefinition>& tools, const std::string& previous_output)
	{
		std::ostringstream out;
		out << "The user request requires action using available tools.\n";
		out << "You MUST return exactly one JSON object of this shape only:\n";
		out << "{\"type\":\"tool_call\",\"name\":\"<tool name>\",\"arguments\":{...}}\n";
		out << "Do not return type=text. Do not return markdown.\n";
		out << "\nAvailable tools:\n";

		for (const ToolDefinition& tool : tools)
		{
			out << "- name: " << tool.name << "\n";

			if (!tool.description.empty())
			{
				out << "  description: " << tool.description << "\n";
			}

			out << "  parameters: " << SerializeJsonCompact(tool.parameters) << "\n";
		}

		out << "\nConversation transcript:\n";
		out << transcript << "\n";

		if (!Trim(previous_output).empty())
		{
			out << "\nPrevious invalid output:\n";
			out << previous_output << "\n";
		}

		out << "\nReturn tool_call JSON only.";
		return out.str();
	}

	inline bool ToolListHasActionTools(const std::vector<ToolDefinition>& tools)
	{
		static const std::set<std::string> kActionTools = {"bash", "edit", "write", "task"};

		for (const ToolDefinition& tool : tools)
		{
			if (kActionTools.find(ToLowerAscii(Trim(tool.name))) != kActionTools.end())
			{
				return true;
			}
		}

		return false;
	}

	inline std::string CollectUserTextLower(const JsonValue& messages)
	{
		if (messages.type != JsonType::Array)
		{
			return "";
		}

		std::ostringstream out;

		for (const JsonValue& entry : messages.array_value)
		{
			if (entry.type != JsonType::Object)
			{
				continue;
			}

			const std::string role = ToLowerAscii(Trim(JsonStringOrEmpty(entry.Find("role"))));

			if (role != "user")
			{
				continue;
			}

			const std::string content = ToLowerAscii(Trim(ExtractContentText(entry.Find("content"))));

			if (content.empty())
			{
				continue;
			}

			out << content << "\n";
		}

		return out.str();
	}

	inline bool RequestLikelyNeedsToolCall(const JsonValue& messages, const std::vector<ToolDefinition>& tools)
	{
		if (!ToolListHasActionTools(tools))
		{
			return false;
		}

		const std::string user_text = CollectUserTextLower(messages);

		if (user_text.empty())
		{
			return false;
		}

		static const std::vector<std::string> kActionHints = {"make ", "create ", "write ", "edit ", "modify ", "update ", "run ", "execute ", "script", "file", "folder", "command", "terminal", "shell", "python", "save ", "delete ", "rename "};

		for (const std::string& hint : kActionHints)
		{
			if (user_text.find(hint) != std::string::npos)
			{
				return true;
			}
		}

		return false;
	}

	inline std::optional<JsonValue> ParseJsonObjectCandidate(const std::string& text)
	{
		const std::string stripped = Trim(StripMarkdownCodeFence(text));

		if (stripped.empty())
		{
			return std::nullopt;
		}

		if (const std::optional<JsonValue> full = ParseJson(stripped); full.has_value() && full->type == JsonType::Object)
		{
			return full;
		}

		const std::size_t first = stripped.find('{');
		const std::size_t last = stripped.rfind('}');

		if (first == std::string::npos || last == std::string::npos || last <= first)
		{
			return std::nullopt;
		}

		const std::string sliced = stripped.substr(first, last - first + 1);

		if (sliced == stripped)
		{
			return std::nullopt;
		}

		if (const std::optional<JsonValue> partial = ParseJson(sliced); partial.has_value() && partial->type == JsonType::Object)
		{
			return partial;
		}

		return std::nullopt;
	}

	inline JsonValue NormalizeToolArguments(const JsonValue* arguments, const std::string& tool_name)
	{
		auto apply_defaults = [&](JsonValue* value)
		{
			if (value == nullptr || value->type != JsonType::Object)
			{
				return;
			}

			if (ToLowerAscii(tool_name) != "bash")
			{
				return;
			}

			JsonValue* command = value->FindMutable("command");

			if ((command == nullptr || command->type != JsonType::String || Trim(command->string_value).empty()))
			{
				if (JsonValue* input = value->FindMutable("input"); input != nullptr && input->type == JsonType::String && !Trim(input->string_value).empty())
				{
					value->object_value["command"] = MakeJsonString(NormalizeBashCommandString(input->string_value));
				}

				command = value->FindMutable("command");
			}

			if (command != nullptr && command->type == JsonType::String)
			{
				command->string_value = NormalizeBashCommandString(command->string_value);
			}

			JsonValue* description = value->FindMutable("description");

			if (description == nullptr || description->type != JsonType::String || Trim(description->string_value).empty())
			{
				value->object_value["description"] = MakeJsonString("Execute requested shell command.");
			}
		};

		if (arguments == nullptr)
		{
			JsonValue out = MakeJsonObject();
			apply_defaults(&out);
			return out;
		}

		if (arguments->type == JsonType::Object)
		{
			JsonValue out = *arguments;
			apply_defaults(&out);
			return out;
		}

		if (arguments->type == JsonType::String)
		{
			const std::string raw = Trim(arguments->string_value);

			if (!raw.empty())
			{
				if (const std::optional<JsonValue> parsed = ParseJson(raw); parsed.has_value() && parsed->type == JsonType::Object)
				{
					JsonValue out = parsed.value();
					apply_defaults(&out);
					return out;
				}
			}

			JsonValue wrapped = MakeJsonObject();

			if (ToLowerAscii(tool_name) == "bash")
			{
				wrapped.object_value["command"] = MakeJsonString(raw);
			}
			else
			{
				wrapped.object_value["input"] = MakeJsonString(raw);
			}

			apply_defaults(&wrapped);
			return wrapped;
		}

		JsonValue wrapped = MakeJsonObject();
		wrapped.object_value["value"] = *arguments;
		apply_defaults(&wrapped);
		return wrapped;
	}

	inline std::optional<ParsedToolEnvelope> ParseToolEnvelopeResponse(const std::string& raw_response, const std::set<std::string>& allowed_tool_names)
	{
		const std::optional<JsonValue> parsed = ParseJsonObjectCandidate(raw_response);

		if (!parsed.has_value())
		{
			return std::nullopt;
		}

		const JsonValue& root = parsed.value();
		const std::string type = ToLowerAscii(Trim(JsonStringOrEmpty(root.Find("type"))));

		if (type == "tool_call" || type == "tool")
		{
			const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("name")));

			if (tool_name.empty())
			{
				return std::nullopt;
			}

			if (!allowed_tool_names.empty() && allowed_tool_names.find(tool_name) == allowed_tool_names.end())
			{
				return std::nullopt;
			}

			JsonValue arguments = NormalizeToolArguments(root.Find("arguments"), tool_name);
			ParsedToolEnvelope out;
			out.is_tool_call = true;
			out.tool_name = tool_name;
			out.tool_arguments_json = SerializeJsonCompact(arguments);
			return out;
		}

		if (type == "text" || type == "assistant")
		{
			const std::string text = Trim(JsonStringOrEmpty(root.Find("text")));

			if (text.empty())
			{
				return std::nullopt;
			}

			ParsedToolEnvelope out;
			out.is_tool_call = false;
			out.text = text;
			return out;
		}

		auto parse_tool_with_name = [&](const std::string& tool_name) -> std::optional<ParsedToolEnvelope>
		{
			const std::string normalized_name = Trim(tool_name);

			if (normalized_name.empty())
			{
				return std::nullopt;
			}

			if (!allowed_tool_names.empty() && allowed_tool_names.find(normalized_name) == allowed_tool_names.end())
			{
				return std::nullopt;
			}

			JsonValue arguments = MakeJsonObject();

			if (const JsonValue* provided_arguments = root.Find("arguments"); provided_arguments != nullptr)
			{
				arguments = NormalizeToolArguments(provided_arguments, normalized_name);
			}
			else if (const JsonValue* provided_arguments = root.Find("args"); provided_arguments != nullptr)
			{
				arguments = NormalizeToolArguments(provided_arguments, normalized_name);
			}

			ParsedToolEnvelope out;
			out.is_tool_call = true;
			out.tool_name = normalized_name;
			out.tool_arguments_json = SerializeJsonCompact(arguments);
			return out;
		};

		if (const std::string direct_type = Trim(JsonStringOrEmpty(root.Find("type"))); !direct_type.empty() && (allowed_tool_names.empty() || allowed_tool_names.find(direct_type) != allowed_tool_names.end()))
		{
			if (const std::optional<ParsedToolEnvelope> parsed_tool = parse_tool_with_name(direct_type); parsed_tool.has_value())
			{
				return parsed_tool;
			}
		}

		if (const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("tool"))); !tool_name.empty())
		{
			if (const std::optional<ParsedToolEnvelope> parsed_tool = parse_tool_with_name(tool_name); parsed_tool.has_value())
			{
				return parsed_tool;
			}
		}

		if (const std::string tool_name = Trim(JsonStringOrEmpty(root.Find("name"))); !tool_name.empty())
		{
			if (const std::optional<ParsedToolEnvelope> parsed_tool = parse_tool_with_name(tool_name); parsed_tool.has_value())
			{
				return parsed_tool;
			}
		}

		if (const JsonValue* tool_calls = root.Find("tool_calls"); tool_calls != nullptr && tool_calls->type == JsonType::Array && !tool_calls->array_value.empty())
		{
			const JsonValue& first = tool_calls->array_value.front();

			if (first.type == JsonType::Object)
			{
				const JsonValue* function = first.Find("function");
				std::string tool_name;
				const JsonValue* arguments_value = nullptr;

				if (function != nullptr && function->type == JsonType::Object)
				{
					tool_name = Trim(JsonStringOrEmpty(function->Find("name")));
					arguments_value = function->Find("arguments");
				}
				else
				{
					tool_name = Trim(JsonStringOrEmpty(first.Find("name")));

					if (tool_name.empty())
					{
						tool_name = Trim(JsonStringOrEmpty(first.Find("tool")));
					}

					arguments_value = first.Find("arguments");
				}

				if (!tool_name.empty() && (allowed_tool_names.empty() || allowed_tool_names.find(tool_name) != allowed_tool_names.end()))
				{
					ParsedToolEnvelope out;
					out.is_tool_call = true;
					out.tool_name = tool_name;
					out.tool_arguments_json = SerializeJsonCompact(NormalizeToolArguments(arguments_value, tool_name));
					return out;
				}
			}
		}

		return std::nullopt;
	}
} // namespace bridge
