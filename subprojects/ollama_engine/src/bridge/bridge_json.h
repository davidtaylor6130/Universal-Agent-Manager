#pragma once

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace bridge
{
	enum class JsonType
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object
	};

	struct JsonValue
	{
		JsonType type = JsonType::Null;
		bool bool_value = false;
		double number_value = 0.0;
		std::string string_value;
		std::vector<JsonValue> array_value;
		std::map<std::string, JsonValue> object_value;

		const JsonValue* Find(const std::string& key) const
		{
			const auto it = object_value.find(key);
			return (it == object_value.end()) ? nullptr : &it->second;
		}

		JsonValue* FindMutable(const std::string& key)
		{
			const auto it = object_value.find(key);
			return (it == object_value.end()) ? nullptr : &it->second;
		}
	};

	inline JsonValue MakeJsonNull()
	{
		JsonValue out;
		out.type = JsonType::Null;
		return out;
	}

	inline JsonValue MakeJsonBool(const bool value)
	{
		JsonValue out;
		out.type = JsonType::Bool;
		out.bool_value = value;
		return out;
	}

	inline JsonValue MakeJsonNumber(const double value)
	{
		JsonValue out;
		out.type = JsonType::Number;
		out.number_value = value;
		return out;
	}

	inline JsonValue MakeJsonString(const std::string& value)
	{
		JsonValue out;
		out.type = JsonType::String;
		out.string_value = value;
		return out;
	}

	inline JsonValue MakeJsonArray()
	{
		JsonValue out;
		out.type = JsonType::Array;
		return out;
	}

	inline JsonValue MakeJsonObject()
	{
		JsonValue out;
		out.type = JsonType::Object;
		return out;
	}

	class JsonParser
	{
	  public:
		explicit JsonParser(const std::string_view input) : input_(input)
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
				JsonValue out = MakeJsonString(ParseString());

				if (error_)
				{
					return {};
				}

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
			JsonValue out = MakeJsonObject();

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

				if (error_)
				{
					break;
				}

				if (!Consume(':'))
				{
					error_ = true;
					break;
				}

				JsonValue value = ParseValue();

				if (error_)
				{
					break;
				}

				out.object_value[key] = std::move(value);
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
			JsonValue out = MakeJsonArray();

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
				JsonValue value = ParseValue();

				if (error_)
				{
					break;
				}

				out.array_value.push_back(std::move(value));
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
			if (MatchLiteral("true"))
			{
				return MakeJsonBool(true);
			}

			if (MatchLiteral("false"))
			{
				return MakeJsonBool(false);
			}

			error_ = true;
			return {};
		}

		JsonValue ParseNull()
		{
			if (!MatchLiteral("null"))
			{
				error_ = true;
				return {};
			}

			return MakeJsonNull();
		}

		JsonValue ParseNumber()
		{
			JsonValue out = MakeJsonNumber(0.0);
			const std::size_t start = pos_;

			if (Peek() == '-')
			{
				++pos_;
			}

			while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
			{
				++pos_;
			}

			if (Peek() == '.')
			{
				++pos_;

				while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
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

				while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
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
				return {};
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

				if (ch != '\\')
				{
					out.push_back(ch);
					continue;
				}

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
				{
					if (pos_ + 4 > input_.size())
					{
						error_ = true;
						return {};
					}

					out += "\\u";
					out.append(input_.substr(pos_, 4));
					pos_ += 4;
					break;
				}

				default:
					error_ = true;
					return {};
				}
			}

			error_ = true;
			return {};
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

		bool Consume(const char ch)
		{
			SkipWhitespace();

			if (pos_ < input_.size() && input_[pos_] == ch)
			{
				++pos_;
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
			while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_])) != 0)
			{
				++pos_;
			}
		}

		std::string_view input_;
		std::size_t pos_ = 0;
		bool error_ = false;
	};

	inline std::optional<JsonValue> ParseJson(const std::string& text)
	{
		JsonParser parser(text);
		return parser.Parse();
	}

	inline void AppendEscapedJsonString(const std::string& value, std::string& out)
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

	inline void AppendJsonCompact(const JsonValue& value, std::string& out)
	{
		switch (value.type)
		{
		case JsonType::Null:
			out += "null";
			return;
		case JsonType::Bool:
			out += value.bool_value ? "true" : "false";
			return;
		case JsonType::Number:
		{
			std::ostringstream number;
			number << std::setprecision(15) << value.number_value;
			out += number.str();
			return;
		}

		case JsonType::String:
			AppendEscapedJsonString(value.string_value, out);
			return;
		case JsonType::Array:
		{
			out.push_back('[');

			for (std::size_t i = 0; i < value.array_value.size(); ++i)
			{
				if (i > 0)
				{
					out.push_back(',');
				}

				AppendJsonCompact(value.array_value[i], out);
			}

			out.push_back(']');
			return;
		}

		case JsonType::Object:
		{
			out.push_back('{');
			bool first = true;

			for (const auto& pair : value.object_value)
			{
				if (!first)
				{
					out.push_back(',');
				}

				first = false;
				AppendEscapedJsonString(pair.first, out);
				out.push_back(':');
				AppendJsonCompact(pair.second, out);
			}

			out.push_back('}');
			return;
		}
		}
	}

	inline std::string SerializeJsonCompact(const JsonValue& value)
	{
		std::string out;
		AppendJsonCompact(value, out);
		return out;
	}

	inline std::string JsonStringOrEmpty(const JsonValue* value)
	{
		if (value == nullptr || value->type != JsonType::String)
		{
			return "";
		}

		return value->string_value;
	}

	inline bool JsonBoolOrDefault(const JsonValue* value, const bool default_value)
	{
		if (value == nullptr || value->type != JsonType::Bool)
		{
			return default_value;
		}

		return value->bool_value;
	}
} // namespace bridge
