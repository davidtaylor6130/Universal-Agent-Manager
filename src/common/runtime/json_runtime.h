#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// <summary>
/// Minimal JSON value model used for Gemini native session parsing and rewriting.
/// </summary>
struct JsonValue
{
	enum class Type
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	Type type = Type::Null;
	bool bool_value = false;
	double number_value = 0.0;
	std::string string_value;
	std::vector<JsonValue> array_value;
	std::unordered_map<std::string, JsonValue> object_value;

	const JsonValue* Find(const std::string& key) const
	{
		const auto it = object_value.find(key);
		return (it == object_value.end()) ? nullptr : &it->second;
	}
};

/// <summary>
/// Lightweight JSON parser for Gemini-native history payloads.
/// </summary>
class JsonParser
{
  public:
	explicit JsonParser(std::string_view input) : input_(input)
	{
	}

	std::optional<JsonValue> Parse()
	{
		SkipWhitespace();
		JsonValue value = ParseValue();

		if (error_)
		{
			return std::nullopt;
		}

		SkipWhitespace();

		if (pos_ != input_.size())
		{
			return std::nullopt;
		}

		return value;
	}

  private:
	JsonValue ParseValue()
	{
		SkipWhitespace();

		if (pos_ >= input_.size())
		{
			error_ = true;
			return {};
		}

		const char ch = input_[pos_];

		if (ch == '{')
		{
			return ParseObject();
		}

		if (ch == '[')
		{
			return ParseArray();
		}

		if (ch == '"')
		{
			JsonValue out;
			out.type = JsonValue::Type::String;
			out.string_value = ParseString();
			return out;
		}

		if (ch == 't' || ch == 'f')
		{
			return ParseBool();
		}

		if (ch == 'n')
		{
			return ParseNull();
		}

		return ParseNumber();
	}

	JsonValue ParseObject()
	{
		JsonValue out;
		out.type = JsonValue::Type::Object;

		if (!Consume('{'))
		{
			error_ = true;
			return {};
		}

		SkipWhitespace();

		if (Consume('}'))
		{
			return out;
		}
		while (!error_)
		{
			SkipWhitespace();

			if (!Consume('"'))
			{
				error_ = true;
				break;
			}

			const std::string key = ParseStringBody();

			if (!Consume(':'))
			{
				error_ = true;
				break;
			}

			JsonValue value = ParseValue();
			out.object_value.emplace(key, std::move(value));
			SkipWhitespace();

			if (Consume('}'))
			{
				break;
			}

			if (!Consume(','))
			{
				error_ = true;
				break;
			}
		}

		return out;
	}

	JsonValue ParseArray()
	{
		JsonValue out;
		out.type = JsonValue::Type::Array;

		if (!Consume('['))
		{
			error_ = true;
			return {};
		}

		SkipWhitespace();

		if (Consume(']'))
		{
			return out;
		}
		while (!error_)
		{
			out.array_value.push_back(ParseValue());
			SkipWhitespace();

			if (Consume(']'))
			{
				break;
			}

			if (!Consume(','))
			{
				error_ = true;
				break;
			}
		}

		return out;
	}

	JsonValue ParseBool()
	{
		JsonValue out;
		out.type = JsonValue::Type::Bool;

		if (MatchLiteral("true"))
		{
			out.bool_value = true;
			return out;
		}

		if (MatchLiteral("false"))
		{
			out.bool_value = false;
			return out;
		}

		error_ = true;
		return {};
	}

	JsonValue ParseNull()
	{
		JsonValue out;
		out.type = JsonValue::Type::Null;

		if (!MatchLiteral("null"))
		{
			error_ = true;
		}

		return out;
	}

	JsonValue ParseNumber()
	{
		JsonValue out;
		out.type = JsonValue::Type::Number;
		const std::size_t start = pos_;

		if (Peek() == '-')
		{
			++pos_;
		}
		while (std::isdigit(static_cast<unsigned char>(Peek())))
		{
			++pos_;
		}

		if (Peek() == '.')
		{
			++pos_;

			while (std::isdigit(static_cast<unsigned char>(Peek())))
			{
				++pos_;
			}
		}

		if (Peek() == 'e' || Peek() == 'E')
		{
			++pos_;

			if (Peek() == '+' || Peek() == '-')
			{
				++pos_;
			}
			while (std::isdigit(static_cast<unsigned char>(Peek())))
			{
				++pos_;
			}
		}

		const std::string token(input_.substr(start, pos_ - start));

		if (token.empty())
		{
			error_ = true;
			return {};
		}

		try
		{
			out.number_value = std::stod(token);
		}
		catch (...)
		{
			error_ = true;
		}

		return out;
	}

	std::string ParseString()
	{
		if (!Consume('"'))
		{
			error_ = true;
			return {};
		}

		return ParseStringBody();
	}

	std::string ParseStringBody()
	{
		std::string out;

		while (pos_ < input_.size())
		{
			const char ch = input_[pos_++];

			if (ch == '"')
			{
				return out;
			}

			if (ch == '\\')
			{
				if (pos_ >= input_.size())
				{
					error_ = true;
					return {};
				}

				const char esc = input_[pos_++];

				switch (esc)
				{
				case '"':
				case '\\':
				case '/':
					out.push_back(esc);
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u':

					if (pos_ + 4 <= input_.size())
					{
						// Keep unicode escape literal if we can't decode it safely here.
						out += "\\u";
						out.append(input_.substr(pos_, 4));
						pos_ += 4;
					}
					else
					{
						error_ = true;
					}

					break;
				default:
					error_ = true;
					break;
				}

				if (error_)
				{
					return {};
				}
			}
			else
			{
				out.push_back(ch);
			}
		}

		error_ = true;
		return {};
	}

	bool Consume(const char expected)
	{
		SkipWhitespace();

		if (pos_ < input_.size() && input_[pos_] == expected)
		{
			++pos_;
			return true;
		}

		return false;
	}

	bool MatchLiteral(const char* literal)
	{
		const std::size_t len = std::strlen(literal);

		if (pos_ + len > input_.size())
		{
			return false;
		}

		if (input_.substr(pos_, len) == literal)
		{
			pos_ += len;
			return true;
		}

		return false;
	}

	char Peek() const
	{
		return (pos_ < input_.size()) ? input_[pos_] : '\0';
	}

	void SkipWhitespace()
	{
		while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])))
		{
			++pos_;
		}
	}

	std::string_view input_;
	std::size_t pos_ = 0;
	bool error_ = false;
};

/// <summary>
/// Parses JSON text into a JsonValue tree.
/// </summary>
static std::optional<JsonValue> ParseJson(const std::string& text)
{
	JsonParser parser(text);
	return parser.Parse();
}

static void AppendJsonEscapedString(const std::string& value, std::string& out)
{
	out.push_back('"');

	for (const unsigned char ch : value)
	{
		switch (ch)
		{
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:

			if (ch < 0x20)
			{
				std::ostringstream esc;
				esc << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
				out += esc.str();
			}
			else
			{
				out.push_back(static_cast<char>(ch));
			}

			break;
		}
	}

	out.push_back('"');
}

static void AppendJsonIndent(const int depth, std::string& out)
{
	for (int i = 0; i < depth; ++i)
	{
		out += "  ";
	}
}

static void AppendJsonValue(const JsonValue& value, std::string& out, const int depth)
{
	switch (value.type)
	{
	case JsonValue::Type::Null:
		out += "null";
		return;
	case JsonValue::Type::Bool:
		out += value.bool_value ? "true" : "false";
		return;
	case JsonValue::Type::Number:
	{
		std::ostringstream number;
		number << std::setprecision(15) << value.number_value;
		out += number.str();
		return;
	}

	case JsonValue::Type::String:
		AppendJsonEscapedString(value.string_value, out);
		return;
	case JsonValue::Type::Array:
	{
		out += "[";

		if (!value.array_value.empty())
		{
			out += "\n";

			for (std::size_t i = 0; i < value.array_value.size(); ++i)
			{
				AppendJsonIndent(depth + 1, out);
				AppendJsonValue(value.array_value[i], out, depth + 1);

				if (i + 1 < value.array_value.size())
				{
					out += ",";
				}

				out += "\n";
			}

			AppendJsonIndent(depth, out);
		}

		out += "]";
		return;
	}

	case JsonValue::Type::Object:
	{
		out += "{";

		if (!value.object_value.empty())
		{
			out += "\n";
			std::vector<std::string> keys;
			keys.reserve(value.object_value.size());

			for (const auto& pair : value.object_value)
			{
				keys.push_back(pair.first);
			}

			std::sort(keys.begin(), keys.end());

			for (std::size_t i = 0; i < keys.size(); ++i)
			{
				const auto it = value.object_value.find(keys[i]);

				if (it == value.object_value.end())
				{
					continue;
				}

				AppendJsonIndent(depth + 1, out);
				AppendJsonEscapedString(it->first, out);
				out += ": ";
				AppendJsonValue(it->second, out, depth + 1);

				if (i + 1 < keys.size())
				{
					out += ",";
				}

				out += "\n";
			}

			AppendJsonIndent(depth, out);
		}

		out += "}";
		return;
	}
	}
}

/// <summary>
/// Serializes a JsonValue tree into normalized JSON text.
/// </summary>
static std::string SerializeJson(const JsonValue& value)
{
	std::string out;
	AppendJsonValue(value, out, 0);
	out.push_back('\n');
	return out;
}

/// <summary>
/// Returns the string value when the node is a JSON string, otherwise an empty string.
/// </summary>
static std::string JsonStringOrEmpty(const JsonValue* value)
{
	if (value == nullptr || value->type != JsonValue::Type::String)
	{
		return "";
	}

	return value->string_value;
}

static std::string JsonTrim(const std::string& value)
{
	const std::size_t start = value.find_first_not_of(" \t\r\n");

	if (start == std::string::npos)
	{
		return "";
	}

	const std::size_t end = value.find_last_not_of(" \t\r\n");
	return value.substr(start, end - start + 1);
}

/// <summary>
/// Extracts Gemini message text from either string, object, or mixed-array JSON content shapes.
/// </summary>
static std::string ExtractGeminiContentText(const JsonValue* value)
{
	if (value == nullptr)
	{
		return "";
	}

	if (value->type == JsonValue::Type::String)
	{
		return value->string_value;
	}

	if (value->type == JsonValue::Type::Array)
	{
		std::ostringstream out;
		bool first = true;

		for (const JsonValue& item : value->array_value)
		{
			std::string piece;

			if (item.type == JsonValue::Type::String)
			{
				piece = item.string_value;
			}
			else if (item.type == JsonValue::Type::Object)
			{
				piece = JsonStringOrEmpty(item.Find("text"));
			}

			piece = JsonTrim(piece);

			if (!piece.empty())
			{
				if (!first)
				{
					out << "\n";
				}

				out << piece;
				first = false;
			}
		}

		return out.str();
	}

	if (value->type == JsonValue::Type::Object)
	{
		return JsonStringOrEmpty(value->Find("text"));
	}

	return "";
}
