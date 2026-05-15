#include "common/config/frontend_actions.h"
#include "common/config/line_value_codec.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace uam
{
	namespace
	{
		constexpr std::size_t kDefaultFrontendActionCount = 5;
		constexpr std::string_view kMetadataSectionName = "metadata";
		constexpr std::string_view kMapSectionName = "map";
		constexpr std::string_view kActionSectionPrefix = "action ";
		constexpr std::string_view kVersionFieldName = "version";
		constexpr std::string_view kSupportedActionMapVersion = "1";
		constexpr std::string_view kLabelFieldName = "label";
		constexpr std::string_view kGroupFieldName = "group";
		constexpr std::string_view kVisibleFieldName = "visible";
		constexpr std::string_view kOrderFieldName = "order";
		constexpr std::string_view kDefaultActionGroup = "general";

		std::optional<bool> ParseVisibilityValue(std::string_view raw_value)
		{
			if (const std::optional<bool> parsed = uam::parse::BoolStrict(raw_value))
			{
				return parsed;
			}

			const std::string value = uam::strings::TrimAndLowerAscii(raw_value);
			if (value == "visible")
			{
				return true;
			}

			if (value == "hidden")
			{
				return false;
			}

			return std::nullopt;
		}

		bool ActionOrderLess(const FrontendAction& a, const FrontendAction& b)
		{
			if (a.order != b.order)
			{
				return a.order < b.order;
			}

			return a.key < b.key;
		}

		bool IsCommentLine(std::string_view trimmed_line)
		{
			return uam::strings::StartsWith(trimmed_line, "#") || uam::strings::StartsWith(trimmed_line, ";") || uam::strings::StartsWith(trimmed_line, "//");
		}

		bool IsVersionField(std::string_view key)
		{
			return uam::strings::TrimmedEqualsIgnoreCase(key, kVersionFieldName);
		}

		bool FailParse(std::string* error_out, std::string_view message)
		{
			if (error_out != nullptr)
			{
				error_out->assign(message);
			}
			return false;
		}

		bool FailParseLine(std::string* error_out, std::string_view message, int line_number)
		{
			return FailParse(error_out, std::string(message) + " on line " + std::to_string(line_number) + ".");
		}

		std::string DecodeActionScalarValue(std::string_view value)
		{
			return uam::UnescapeLineValueBody(uam::strings::Trim(value), uam::UnknownLineEscapePolicy::DropBackslash);
		}

		FrontendAction MakeDefaultAction(std::string_view key, std::string_view label, std::string_view group, int order)
		{
			FrontendAction action;
			action.key = std::string(key);
			action.label = std::string(label);
			action.group = std::string(group);
			action.visible = true;
			action.order = order;
			return action;
		}

		template <typename ActionMap>
		auto FindActionByKey(ActionMap& action_map, std::string_view key)
		{
			return std::ranges::find_if(action_map.actions, [key](const FrontendAction& action) { return action.key == key; });
		}

		template <typename ActionMap>
		auto* ActionOrNull(ActionMap& action_map, decltype(action_map.actions.begin()) found)
		{
			return found == action_map.actions.end() ? nullptr : &*found;
		}

		bool AssignField(FrontendAction& action, std::string_view field, std::string_view raw_value, int line_number, std::string* error_out)
		{
			const std::string normalized_field = uam::strings::TrimAndLowerAscii(field);
			const std::string value = DecodeActionScalarValue(raw_value);

			if (normalized_field == kLabelFieldName)
			{
				action.label = value;
				return true;
			}

			if (normalized_field == kGroupFieldName)
			{
				action.group = value;
				return true;
			}

			if (normalized_field == kVisibleFieldName)
			{
				const std::optional<bool> parsed = ParseVisibilityValue(value);
				if (!parsed)
				{
					return FailParseLine(error_out, "Invalid boolean value", line_number);
				}

				action.visible = *parsed;
				return true;
			}

			if (normalized_field == kOrderFieldName)
			{
				const std::optional<int> parsed = uam::parse::IntStrict(value);
				if (!parsed)
				{
					return FailParseLine(error_out, "Invalid integer value", line_number);
				}

				action.order = *parsed;
				return true;
			}

			action.properties[normalized_field] = value;
			return true;
		}

		void NormalizeAction(FrontendAction& action)
		{
			action.key = uam::strings::Trim(action.key);
			action.label = uam::strings::Trim(action.label);
			action.group = uam::strings::Trim(action.group);

			if (action.key.empty())
			{
				return;
			}

			if (action.label.empty())
			{
				action.label = action.key;
			}

			if (action.group.empty())
			{
				action.group = kDefaultActionGroup;
			}
		}

	} // namespace

	FrontendActionMap DefaultFrontendActionMap()
	{
		FrontendActionMap out;
		out.actions.reserve(kDefaultFrontendActionCount);
		out.actions.push_back(MakeDefaultAction("create_chat", "Create Chat", "chat", 10));
		out.actions.push_back(MakeDefaultAction("delete_chat", "Delete Chat", "chat", 20));
		out.actions.push_back(MakeDefaultAction("send_prompt", "Send Prompt", "composer", 30));
		out.actions.push_back(MakeDefaultAction("edit_resubmit", "Edit and Resubmit", "conversation", 40));
		out.actions.push_back(MakeDefaultAction("refresh_history", "Refresh History", "history", 50));
		NormalizeFrontendActionMap(out);
		return out;
	}

	FrontendAction* FindAction(FrontendActionMap& action_map, std::string_view key)
	{
		return ActionOrNull(action_map, FindActionByKey(action_map, key));
	}

	const FrontendAction* FindAction(const FrontendActionMap& action_map, std::string_view key)
	{
		return ActionOrNull(action_map, FindActionByKey(action_map, key));
	}

	void NormalizeFrontendActionMap(FrontendActionMap& action_map)
	{
		std::unordered_map<std::string, FrontendAction> by_key;

		for (FrontendAction action : action_map.actions)
		{
			NormalizeAction(action);

			if (action.key.empty())
			{
				continue;
			}

			by_key[action.key] = std::move(action);
		}

		std::vector<FrontendAction> normalized;
		normalized.reserve(by_key.size());

		for (auto& pair : by_key)
		{
			normalized.push_back(std::move(pair.second));
		}

		std::ranges::sort(normalized, ActionOrderLess);

		action_map.actions = std::move(normalized);
	}

	bool ParseFrontendActionMap(const std::string& text, FrontendActionMap& out_map, std::string* error_out)
	{
		out_map = FrontendActionMap{};
		if (error_out != nullptr)
		{
			error_out->clear();
		}

		FrontendActionMap parsed;
		FrontendAction* current_action = nullptr;
		enum class Section
		{
			None,
			Metadata,
			Action
		};

		Section current_section = Section::None;

		std::istringstream input(text);
		std::string line;
		int line_number = 0;

		while (std::getline(input, line))
		{
			++line_number;
			const std::string trimmed = uam::strings::Trim(line);

			if (trimmed.empty() || IsCommentLine(trimmed))
			{
				continue;
			}

			if (trimmed.front() == '[' && trimmed.back() == ']')
			{
				const std::string section = uam::strings::Trim(trimmed.substr(1, trimmed.size() - 2));
				const std::string section_lower = uam::strings::ToLowerAscii(section);
				current_action = nullptr;

				if (section_lower == kMetadataSectionName || section_lower == kMapSectionName)
				{
					current_section = Section::Metadata;
					continue;
				}

				if (uam::strings::StartsWith(section_lower, kActionSectionPrefix))
				{
					const std::string key = uam::strings::Trim(section.substr(kActionSectionPrefix.size()));

					if (key.empty())
					{
						return FailParseLine(error_out, "Missing action key", line_number);
					}

					parsed.actions.push_back(FrontendAction{});
					current_action = &parsed.actions.back();
					current_action->key = key;
					current_section = Section::Action;
					continue;
				}

				return FailParseLine(error_out, "Unknown section header", line_number);
			}

			const std::size_t equals_at = trimmed.find('=');

			if (equals_at == std::string::npos)
			{
				return FailParseLine(error_out, "Expected key=value", line_number);
			}

			const std::string key = uam::strings::Trim(trimmed.substr(0, equals_at));
			const std::string value = trimmed.substr(equals_at + 1);

			if (key.empty())
			{
				return FailParseLine(error_out, "Missing key", line_number);
			}

			if (current_section == Section::Metadata)
			{
				parsed.metadata[key] = DecodeActionScalarValue(value);
				continue;
			}

			if (current_section == Section::Action && current_action != nullptr)
			{
				std::string local_error;
				if (!AssignField(*current_action, key, value, line_number, &local_error))
				{
					if (error_out != nullptr)
					{
						*error_out = local_error;
					}

					return false;
				}

				continue;
			}

			if (IsVersionField(key))
			{
				const std::string version = uam::strings::Trim(DecodeActionScalarValue(value));

				if (version != kSupportedActionMapVersion)
				{
					return FailParseLine(error_out, "Unsupported action map version", line_number);
				}

				continue;
			}

			parsed.metadata[key] = DecodeActionScalarValue(value);
		}

		NormalizeFrontendActionMap(parsed);
		out_map = std::move(parsed);
		return true;
	}

	std::string SerializeFrontendActionMap(const FrontendActionMap& action_map)
	{
		std::ostringstream out;
		out << "# Universal frontend action map\n";
		out << kVersionFieldName << " = " << kSupportedActionMapVersion << "\n";
		out << "\n";

		if (!action_map.metadata.empty())
		{
			out << "[metadata]\n";

			for (const auto& [key, value] : action_map.metadata)
			{
				if (IsVersionField(key))
				{
					continue;
				}

				out << key << " = " << uam::EscapeLineValueBody(value) << "\n";
			}

			out << "\n";
		}

		std::vector<FrontendAction> sorted_actions = action_map.actions;
		std::ranges::sort(sorted_actions, ActionOrderLess);

		for (const FrontendAction& action : sorted_actions)
		{
			if (action.key.empty())
			{
				continue;
			}

			out << "[action " << action.key << "]\n";
			out << "label = " << uam::EscapeLineValueBody(action.label) << "\n";
			out << "group = " << uam::EscapeLineValueBody(action.group) << "\n";
			out << "visible = " << (action.visible ? "true" : "false") << "\n";
			out << "order = " << action.order << "\n";

			for (const auto& [key, value] : action.properties)
			{
				out << key << " = " << uam::EscapeLineValueBody(value) << "\n";
			}

			out << "\n";
		}

		return out.str();
	}

	bool LoadFrontendActionMap(const std::filesystem::path& path, FrontendActionMap& out_map, std::string* error_out)
	{
		out_map = FrontendActionMap{};
		if (error_out != nullptr)
		{
			error_out->clear();
		}

		std::string text;
		if (!uam::io::TryReadTextFile(path, text))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open '" + path.string() + "' for reading.";
			}

			return false;
		}

		return ParseFrontendActionMap(text, out_map, error_out);
	}

	bool SaveFrontendActionMap(const std::filesystem::path& path, const FrontendActionMap& action_map, std::string* error_out)
	{
		if (error_out != nullptr)
		{
			error_out->clear();
		}

		std::error_code ec;
		const std::filesystem::path parent = path.parent_path();

		if (!parent.empty())
		{
			if (!uam::paths::CreateDirectoriesNoThrow(parent, &ec))
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create directory '" + parent.string() + "': " + ec.message();
				}

				return false;
			}
		}

		const std::string text = SerializeFrontendActionMap(action_map);
		if (!uam::io::WriteTextFile(path, text))
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to write '" + path.string() + "'.";
			}

			return false;
		}

		return true;
	}

} // namespace uam
