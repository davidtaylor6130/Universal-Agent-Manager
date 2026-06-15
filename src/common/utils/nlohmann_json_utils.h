#pragma once

#include "common/utils/string_utils.h"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace uam::nlohmann_json
{
	inline const nlohmann::json* FindField(const nlohmann::json& object, std::string_view key)
	{
		if (!object.is_object())
		{
			return nullptr;
		}

		const auto it = object.find(std::string(key));
		return it == object.end() ? nullptr : &*it;
	}

	inline const nlohmann::json* FindStringField(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindField(object, key);
		if (value == nullptr || !value->is_string())
		{
			return nullptr;
		}

		return value;
	}

	inline bool HasStringField(const nlohmann::json& object, std::string_view key)
	{
		return FindStringField(object, key) != nullptr;
	}

	inline std::optional<bool> BoolValueStrict(const nlohmann::json& value)
	{
		return value.is_boolean() ? std::optional<bool>{value.get<bool>()} : std::nullopt;
	}

	inline std::optional<bool> BoolFieldStrict(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindField(object, key);
		return value == nullptr ? std::nullopt : BoolValueStrict(*value);
	}

	inline std::optional<int> IntValueStrict(const nlohmann::json& value)
	{
		if (value.is_number_unsigned())
		{
			const std::uint64_t parsed = value.get<std::uint64_t>();
			if (parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
			{
				return std::nullopt;
			}
			return static_cast<int>(parsed);
		}
		if (value.is_number_integer())
		{
			const std::int64_t parsed = value.get<std::int64_t>();
			if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max())
			{
				return std::nullopt;
			}
			return static_cast<int>(parsed);
		}
		return std::nullopt;
	}

	inline std::optional<int> IntFieldStrict(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindField(object, key);
		return value == nullptr ? std::nullopt : IntValueStrict(*value);
	}

	inline const nlohmann::json* FindArrayField(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindField(object, key);
		if (value == nullptr || !value->is_array())
		{
			return nullptr;
		}

		return value;
	}

	inline nlohmann::json ArrayFieldOrEmpty(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindArrayField(object, key);
		return value == nullptr ? nlohmann::json::array() : *value;
	}

	inline const nlohmann::json* FindObjectField(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindField(object, key);
		if (value == nullptr || !value->is_object())
		{
			return nullptr;
		}

		return value;
	}

	inline nlohmann::json ObjectFieldOrEmpty(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindObjectField(object, key);
		return value == nullptr ? nlohmann::json::object() : *value;
	}

	inline nlohmann::json StringOrNull(std::string_view value)
	{
		return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(value));
	}

	inline nlohmann::json IntOrNull(bool has_value, int value)
	{
		return has_value ? nlohmann::json(value) : nlohmann::json(nullptr);
	}

	inline nlohmann::json ValueOrNull(const nlohmann::json* value)
	{
		return value == nullptr ? nlohmann::json(nullptr) : *value;
	}

	inline std::vector<std::string> StringArrayValue(const nlohmann::json& value)
	{
		std::vector<std::string> values;
		if (!value.is_array())
		{
			return values;
		}

		values.reserve(value.size());
		for (const nlohmann::json& item : value)
		{
			if (item.is_string())
			{
				values.push_back(item.get_ref<const std::string&>());
			}
		}
		return values;
	}

	inline std::vector<std::string> StringArrayField(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindArrayField(object, key);
		return value == nullptr ? std::vector<std::string>{} : StringArrayValue(*value);
	}

	inline std::vector<std::string> StringListValue(const nlohmann::json& value)
	{
		if (value.is_string())
		{
			return {value.get_ref<const std::string&>()};
		}
		return StringArrayValue(value);
	}

	inline std::optional<std::string> TrimmedStringScalarValue(const nlohmann::json& value)
	{
		if (!value.is_string())
		{
			return std::nullopt;
		}

		const std::string trimmed = uam::strings::Trim(value.get_ref<const std::string&>());
		return trimmed.empty() ? std::nullopt : std::optional<std::string>{trimmed};
	}

	inline bool AppendTrimmedStringScalarValue(std::vector<std::string>& values, const nlohmann::json& value)
	{
		std::optional<std::string> trimmed = TrimmedStringScalarValue(value);
		if (!trimmed)
		{
			return false;
		}

		values.push_back(std::move(*trimmed));
		return true;
	}

	inline std::vector<std::string> TrimmedStringArrayField(const nlohmann::json& object, std::string_view key)
	{
		std::vector<std::string> values;
		const nlohmann::json* items = FindArrayField(object, key);
		if (items == nullptr)
		{
			return values;
		}

		values.reserve(items->size());
		for (const nlohmann::json& item : *items)
		{
			AppendTrimmedStringScalarValue(values, item);
		}
		return values;
	}

	inline std::string_view StringViewOrEmpty(const nlohmann::json& object, std::string_view key)
	{
		const nlohmann::json* value = FindStringField(object, key);
		return value == nullptr ? std::string_view{} : std::string_view{value->get_ref<const std::string&>()};
	}

	inline std::string_view TrimmedStringViewOrEmpty(const nlohmann::json& object, std::string_view key)
	{
		return uam::strings::TrimAsciiView(StringViewOrEmpty(object, key));
	}

	inline std::string TrimmedStringValue(const nlohmann::json& object, std::initializer_list<std::string_view> keys)
	{
		for (std::string_view key : keys)
		{
			const std::string_view value = TrimmedStringViewOrEmpty(object, key);
			if (!value.empty())
			{
				return std::string(value);
			}
		}

		return {};
	}

	inline std::string TrimmedStringValueOr(const nlohmann::json& object, std::string_view key, std::string_view fallback)
	{
		const std::string_view value = TrimmedStringViewOrEmpty(object, key);
		return value.empty() ? uam::strings::Trim(fallback) : std::string(value);
	}
} // namespace uam::nlohmann_json
