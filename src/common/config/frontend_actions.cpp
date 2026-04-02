#include "frontend_actions.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace uam
{
	namespace
	{

		std::string Trim(const std::string& value)
		{
			const std::size_t first = value.find_first_not_of(" \t\r\n");

			if (first == std::string::npos)
			{
				return "";
			}

			const std::size_t last = value.find_last_not_of(" \t\r\n");
			return value.substr(first, last - first + 1);
		}

		std::string ToLower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		}

		bool StartsWith(const std::string& value, const std::string& prefix)
		{
			return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
		}

		std::string EncodeValue(const std::string& value)
		{
			std::string encoded;
			encoded.reserve(value.size());

			for (const char ch : value)
			{
				switch (ch)
				{
				case '\\':
					encoded += "\\\\";
					break;
				case '\n':
					encoded += "\\n";
					break;
				case '\r':
					encoded += "\\r";
					break;
				case '\t':
					encoded += "\\t";
					break;
				default:
					encoded.push_back(ch);
					break;
				}
			}

			return encoded;
		}

		std::string DecodeValue(const std::string& value)
		{
			std::string decoded;
			decoded.reserve(value.size());

			for (std::size_t i = 0; i < value.size(); ++i)
			{
				const char ch = value[i];

				if (ch != '\\' || i + 1 >= value.size())
				{
					decoded.push_back(ch);
					continue;
				}

				const char next = value[++i];

				switch (next)
				{
				case 'n':
					decoded.push_back('\n');
					break;
				case 'r':
					decoded.push_back('\r');
					break;
				case 't':
					decoded.push_back('\t');
					break;
				case '\\':
					decoded.push_back('\\');
					break;
				default:
					decoded.push_back(next);
					break;
				}
			}

			return decoded;
		}

		bool ParseBool(const std::string& raw_value, bool* out_value)
		{
			const std::string value = ToLower(Trim(raw_value));

			if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "visible")
			{
				*out_value = true;
				return true;
			}

			if (value == "0" || value == "false" || value == "no" || value == "off" || value == "hidden")
			{
				*out_value = false;
				return true;
			}

			return false;
		}

		bool ParseInt(const std::string& raw_value, int* out_value)
		{
			const std::string value = Trim(raw_value);

			if (value.empty())
			{
				return false;
			}

			std::size_t index = 0;

			try
			{
				const int parsed = std::stoi(value, &index, 10);

				if (index != value.size())
				{
					return false;
				}

				*out_value = parsed;
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		bool IsCommentLine(const std::string& trimmed_line)
		{
			return trimmed_line.rfind("#", 0) == 0 || trimmed_line.rfind(";", 0) == 0 || trimmed_line.rfind("//", 0) == 0;
		}

		FrontendAction MakeDefaultAction(const std::string& key, const std::string& label, const std::string& group, const int order)
		{
			FrontendAction action;
			action.key = key;
			action.label = label;
			action.group = group;
			action.visible = true;
			action.order = order;
			return action;
		}

		void AssignField(FrontendAction& action, const std::string& field, const std::string& raw_value, const int line_number, std::string* error_out)
		{
			const std::string normalized_field = ToLower(Trim(field));
			const std::string value = DecodeValue(Trim(raw_value));

			if (normalized_field == "label")
			{
				action.label = value;
				return;
			}

			if (normalized_field == "group")
			{
				action.group = value;
				return;
			}

			if (normalized_field == "visible")
			{
				bool parsed = true;

				if (!ParseBool(value, &parsed))
				{
					if (error_out != nullptr)
					{
						*error_out = "Invalid boolean value on line " + std::to_string(line_number) + ".";
					}

					return;
				}

				action.visible = parsed;
				return;
			}

			if (normalized_field == "order")
			{
				int parsed = 0;

				if (!ParseInt(value, &parsed))
				{
					if (error_out != nullptr)
					{
						*error_out = "Invalid integer value on line " + std::to_string(line_number) + ".";
					}

					return;
				}

				action.order = parsed;
				return;
			}

			action.properties[normalized_field] = value;
		}

		void NormalizeAction(FrontendAction& action)
		{
			action.key = Trim(action.key);
			action.label = Trim(action.label);
			action.group = Trim(action.group);

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
				action.group = "general";
			}
		}

	} // namespace

	FrontendActionMap DefaultFrontendActionMap()
	{
		FrontendActionMap out;
		out.actions.push_back(MakeDefaultAction("create_chat", "Create Chat", "chat", 10));
		out.actions.push_back(MakeDefaultAction("delete_chat", "Delete Chat", "chat", 20));
		out.actions.push_back(MakeDefaultAction("send_prompt", "Send Prompt", "composer", 30));
		out.actions.push_back(MakeDefaultAction("edit_resubmit", "Edit and Resubmit", "conversation", 40));
		out.actions.push_back(MakeDefaultAction("refresh_history", "Refresh History", "history", 50));
		NormalizeFrontendActionMap(out);
		return out;
	}

	FrontendAction* FindAction(FrontendActionMap& action_map, const std::string& key)
	{
		for (FrontendAction& action : action_map.actions)
		{
			if (action.key == key)
			{
				return &action;
			}
		}

		return nullptr;
	}

	const FrontendAction* FindAction(const FrontendActionMap& action_map, const std::string& key)
	{
		for (const FrontendAction& action : action_map.actions)
		{
			if (action.key == key)
			{
				return &action;
			}
		}

		return nullptr;
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

		auto sort_actions_by_order_then_key = [](const FrontendAction& a, const FrontendAction& b)
		{
			if (a.order != b.order)
			{
				return a.order < b.order;
			}

			return a.key < b.key;
		};

		std::sort(normalized.begin(), normalized.end(), sort_actions_by_order_then_key);

		action_map.actions = std::move(normalized);
	}

	bool ParseFrontendActionMap(const std::string& text, FrontendActionMap& out_map, std::string* error_out)
	{
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
			const std::string trimmed = Trim(line);

			if (trimmed.empty() || IsCommentLine(trimmed))
			{
				continue;
			}

			if (trimmed.front() == '[' && trimmed.back() == ']')
			{
				const std::string section = Trim(trimmed.substr(1, trimmed.size() - 2));
				const std::string section_lower = ToLower(section);
				current_action = nullptr;

				if (section_lower == "metadata" || section_lower == "map")
				{
					current_section = Section::Metadata;
					continue;
				}

				if (StartsWith(section_lower, "action "))
				{
					const std::string key = Trim(section.substr(7));

					if (key.empty())
					{
						if (error_out != nullptr)
						{
							*error_out = "Missing action key on line " + std::to_string(line_number) + ".";
						}

						return false;
					}

					parsed.actions.push_back(FrontendAction{});
					current_action = &parsed.actions.back();
					current_action->key = key;
					current_section = Section::Action;
					continue;
				}

				if (error_out != nullptr)
				{
					*error_out = "Unknown section header on line " + std::to_string(line_number) + ".";
				}

				return false;
			}

			const std::size_t equals_at = trimmed.find('=');

			if (equals_at == std::string::npos)
			{
				if (error_out != nullptr)
				{
					*error_out = "Expected key=value on line " + std::to_string(line_number) + ".";
				}

				return false;
			}

			const std::string key = Trim(trimmed.substr(0, equals_at));
			const std::string value = trimmed.substr(equals_at + 1);

			if (key.empty())
			{
				if (error_out != nullptr)
				{
					*error_out = "Missing key on line " + std::to_string(line_number) + ".";
				}

				return false;
			}

			if (current_section == Section::Metadata)
			{
				parsed.metadata[key] = DecodeValue(Trim(value));
				continue;
			}

			if (current_section == Section::Action && current_action != nullptr)
			{
				std::string local_error;
				AssignField(*current_action, key, value, line_number, &local_error);

				if (!local_error.empty())
				{
					if (error_out != nullptr)
					{
						*error_out = local_error;
					}

					return false;
				}

				continue;
			}

			if (ToLower(key) == "version")
			{
				const std::string version = Trim(DecodeValue(Trim(value)));

				if (version != "1")
				{
					if (error_out != nullptr)
					{
						*error_out = "Unsupported action map version on line " + std::to_string(line_number) + ".";
					}

					return false;
				}

				continue;
			}

			parsed.metadata[key] = DecodeValue(Trim(value));
		}

		NormalizeFrontendActionMap(parsed);
		out_map = std::move(parsed);
		return true;
	}

	std::string SerializeFrontendActionMap(const FrontendActionMap& action_map)
	{
		std::ostringstream out;
		out << "# Universal frontend action map\n";
		out << "version = 1\n";
		out << "\n";

		if (!action_map.metadata.empty())
		{
			out << "[metadata]\n";

			for (const auto& [key, value] : action_map.metadata)
			{
				if (ToLower(key) == "version")
				{
					continue;
				}

				out << key << " = " << EncodeValue(value) << "\n";
			}

			out << "\n";
		}

		std::vector<FrontendAction> sorted_actions = action_map.actions;
		auto sort_actions_by_order_then_key = [](const FrontendAction& a, const FrontendAction& b)
		{
			if (a.order != b.order)
			{
				return a.order < b.order;
			}

			return a.key < b.key;
		};
		std::sort(sorted_actions.begin(), sorted_actions.end(), sort_actions_by_order_then_key);

		for (const FrontendAction& action : sorted_actions)
		{
			if (action.key.empty())
			{
				continue;
			}

			out << "[action " << action.key << "]\n";
			out << "label = " << EncodeValue(action.label) << "\n";
			out << "group = " << EncodeValue(action.group) << "\n";
			out << "visible = " << (action.visible ? "true" : "false") << "\n";
			out << "order = " << action.order << "\n";

			for (const auto& [key, value] : action.properties)
			{
				out << key << " = " << EncodeValue(value) << "\n";
			}

			out << "\n";
		}

		return out.str();
	}

	bool LoadFrontendActionMap(const std::filesystem::path& path, FrontendActionMap& out_map, std::string* error_out)
	{
		std::ifstream input(path, std::ios::binary);

		if (!input.good())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open '" + path.string() + "' for reading.";
			}

			return false;
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		return ParseFrontendActionMap(buffer.str(), out_map, error_out);
	}

	bool SaveFrontendActionMap(const std::filesystem::path& path, const FrontendActionMap& action_map, std::string* error_out)
	{
		std::error_code ec;
		const std::filesystem::path parent = path.parent_path();

		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);

			if (ec)
			{
				if (error_out != nullptr)
				{
					*error_out = "Failed to create directory '" + parent.string() + "': " + ec.message();
				}

				return false;
			}
		}

		std::ofstream output(path, std::ios::binary | std::ios::trunc);

		if (!output.good())
		{
			if (error_out != nullptr)
			{
				*error_out = "Failed to open '" + path.string() + "' for writing.";
			}

			return false;
		}

		output << SerializeFrontendActionMap(action_map);

		if (!output.good())
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
