#pragma once

#include "common/utils/nlohmann_json_utils.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace uam::acp_content
{
	inline constexpr const char* kTypeField = "type";
	inline constexpr const char* kTextField = "text";
	inline constexpr const char* kTextType = "text";
	inline constexpr const char* kTextElementsField = "text_elements";

	inline nlohmann::json TextPart(std::string_view text)
	{
		return {
		    {kTypeField, kTextType},
		    {kTextField, std::string(text)},
		};
	}

	inline nlohmann::json CodexTextInputPart(std::string_view text)
	{
		nlohmann::json part = TextPart(text);
		part[kTextElementsField] = nlohmann::json::array();
		return part;
	}

	inline bool HasTextField(const nlohmann::json& value)
	{
		return uam::nlohmann_json::HasStringField(value, kTextField);
	}

	inline std::string_view TextFieldViewOrEmpty(const nlohmann::json& value)
	{
		return uam::nlohmann_json::StringViewOrEmpty(value, kTextField);
	}

	inline std::string TextFieldOrEmpty(const nlohmann::json& value)
	{
		return std::string(TextFieldViewOrEmpty(value));
	}

	inline std::size_t SumTextFieldSizes(const nlohmann::json& values)
	{
		if (!values.is_array())
		{
			return 0;
		}

		std::size_t total = 0;
		for (const nlohmann::json& value : values)
		{
			total += TextFieldViewOrEmpty(value).size();
		}
		return total;
	}
} // namespace uam::acp_content
