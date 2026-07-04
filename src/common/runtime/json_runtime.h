#pragma once

#include <optional>
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
	JsonValue Object();
	JsonValue Array();
	JsonValue String(std::string_view text);
	JsonValue Number(double number);
	JsonValue Bool(bool flag);
	void SetValue(JsonValue& object, std::string_view key, JsonValue value);
	void PushValue(JsonValue& array, JsonValue value);
	void SetString(JsonValue& object, std::string_view key, std::string_view text);
	void SetNumber(JsonValue& object, std::string_view key, double number);
	void SetBool(JsonValue& object, std::string_view key, bool flag);
	const JsonValue* ArrayOrNull(const JsonValue* value);
} // namespace uam::json

std::optional<JsonValue> ParseJson(std::string_view text);
std::string SerializeJson(const JsonValue& value);
std::string JsonStringOrEmpty(const JsonValue* value);
std::string ExtractGeminiContentText(const JsonValue* value);
double JsonNumberOrDefault(const JsonValue* value, double fallback);
bool JsonBoolOrDefault(const JsonValue* value, bool fallback);
