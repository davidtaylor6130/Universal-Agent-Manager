#pragma once

#include "common/utils/parse_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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

	JsonValue* Find(std::string_view key)
	{
		const auto it = object_value.find(std::string(key));
		return (it == object_value.end()) ? nullptr : &it->second;
	}

	const JsonValue* Find(std::string_view key) const
	{
		const auto it = object_value.find(std::string(key));
		return (it == object_value.end()) ? nullptr : &it->second;
	}
};

namespace uam::json
{

	inline JsonValue Object()
	{
		JsonValue value;
		value.type = JsonValue::Type::Object;
		return value;
	}

	inline JsonValue Array()
	{
		JsonValue value;
		value.type = JsonValue::Type::Array;
		return value;
	}

	inline JsonValue String(std::string_view text)
	{
		JsonValue value;
		value.type = JsonValue::Type::String;
		value.string_value.assign(text);
		return value;
	}

	inline JsonValue Number(double number)
	{
		JsonValue value;
		value.type = JsonValue::Type::Number;
		value.number_value = number;
		return value;
	}

	inline JsonValue Bool(bool flag)
	{
		JsonValue value;
		value.type = JsonValue::Type::Bool;
		value.bool_value = flag;
		return value;
	}

	inline void SetValue(JsonValue& object, std::string_view key, JsonValue value)
	{
		object.object_value[std::string(key)] = std::move(value);
	}

	inline void PushValue(JsonValue& array, JsonValue value)
	{
		array.array_value.push_back(std::move(value));
	}

	inline void SetString(JsonValue& object, std::string_view key, std::string_view text)
	{
		SetValue(object, key, String(text));
	}

	inline void SetNumber(JsonValue& object, std::string_view key, double number)
	{
		SetValue(object, key, Number(number));
	}

	inline void SetBool(JsonValue& object, std::string_view key, bool flag)
	{
		SetValue(object, key, Bool(flag));
	}

	inline const JsonValue* ArrayOrNull(const JsonValue* value)
	{
		return (value != nullptr && value->type == JsonValue::Type::Array) ? value : nullptr;
	}

} // namespace uam::json

namespace uam::json_runtime_detail
{

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
			SkipBOM();
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
		void SkipBOM()
		{
			if (pos_ + 2 < input_.size() && static_cast<unsigned char>(input_[pos_]) == 0xEF && static_cast<unsigned char>(input_[pos_ + 1]) == 0xBB && static_cast<unsigned char>(input_[pos_ + 2]) == 0xBF)
			{
				pos_ += 3;
			}
		}

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

			const char integer_head = Peek();
			if (integer_head == '0')
			{
				++pos_;
				if (uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())))
				{
					error_ = true;
					return {};
				}
			}
			else if (integer_head >= '1' && integer_head <= '9')
			{
				do
				{
					++pos_;
				} while (uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())));
			}
			else
			{
				error_ = true;
				return {};
			}

			if (Peek() == '.')
			{
				++pos_;
				if (!uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())))
				{
					error_ = true;
					return {};
				}

				do
				{
					++pos_;
				} while (uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())));
			}

			if (Peek() == 'e' || Peek() == 'E')
			{
				++pos_;

				if (Peek() == '+' || Peek() == '-')
				{
					++pos_;
				}
				if (!uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())))
				{
					error_ = true;
					return {};
				}

				do
				{
					++pos_;
				} while (uam::strings::IsAsciiDigit(static_cast<unsigned char>(Peek())));
			}

			const std::string_view token = input_.substr(start, pos_ - start);
			const std::optional<double> parsed = uam::parse::DoubleStrict(token);
			if (!parsed)
			{
				error_ = true;
				return {};
			}

			out.number_value = *parsed;
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
					if (static_cast<unsigned char>(ch) < 0x20)
					{
						error_ = true;
						return {};
					}
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
			while (pos_ < input_.size() && uam::strings::IsAsciiSpace(static_cast<unsigned char>(input_[pos_])))
			{
				++pos_;
			}
		}

		std::string_view input_;
		std::size_t pos_ = 0;
		bool error_ = false;
	};

} // namespace uam::json_runtime_detail

/// <summary>
/// Parses JSON text into a JsonValue tree.
/// </summary>
inline std::optional<JsonValue> ParseJson(std::string_view text)
{
	uam::json_runtime_detail::JsonParser parser(text);
	return parser.Parse();
}

namespace uam::json_runtime_detail
{

	inline void AppendJsonEscapedString(const std::string& value, std::string& out)
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

	inline void AppendJsonIndent(int depth, std::string& out)
	{
		for (int i = 0; i < depth; ++i)
		{
			out += "  ";
		}
	}

	inline void AppendJsonValue(const JsonValue& value, std::string& out, int depth)
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

				std::ranges::sort(keys);

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

} // namespace uam::json_runtime_detail

/// <summary>
/// Serializes a JsonValue tree into normalized JSON text.
/// </summary>
inline std::string SerializeJson(const JsonValue& value)
{
	std::string out;
	uam::json_runtime_detail::AppendJsonValue(value, out, 0);
	out.push_back('\n');
	return out;
}

/// <summary>
/// Returns the string value when the node is a JSON string, otherwise an empty string.
/// </summary>
inline std::string JsonStringOrEmpty(const JsonValue* value)
{
	if (value == nullptr || value->type != JsonValue::Type::String)
	{
		return "";
	}

	return value->string_value;
}

/// <summary>
/// Extracts Gemini message text from either string, object, or mixed-array JSON content shapes.
/// </summary>
inline std::string ExtractGeminiContentText(const JsonValue* value)
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
		std::vector<std::string> content_pieces;

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

			piece = uam::strings::Trim(piece);

			if (!piece.empty())
			{
				content_pieces.push_back(piece);
			}
		}

		return uam::strings::JoinNonEmpty(content_pieces, "\n");
	}

	if (value->type == JsonValue::Type::Object)
	{
		return JsonStringOrEmpty(value->Find("text"));
	}

	return "";
}

inline double JsonNumberOrDefault(const JsonValue* value, double fallback)
{
	if (value == nullptr)
	{
		return fallback;
	}

	if (value->type == JsonValue::Type::Number)
	{
		return value->number_value;
	}

	if (value->type == JsonValue::Type::String)
	{
		return uam::parse::DoubleOr(value->string_value, fallback);
	}

	return fallback;
}

inline bool JsonBoolOrDefault(const JsonValue* value, bool fallback)
{
	if (value == nullptr)
	{
		return fallback;
	}

	if (value->type == JsonValue::Type::Bool)
	{
		return value->bool_value;
	}

	if (value->type == JsonValue::Type::Number)
	{
		return value->number_value != 0.0;
	}

	if (value->type == JsonValue::Type::String)
	{
		return uam::parse::BoolOr(value->string_value, fallback);
	}

	return fallback;
}
