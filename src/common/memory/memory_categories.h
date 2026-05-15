#pragma once

#include "common/utils/range_utils.h"

#include <string>
#include <string_view>
#include <vector>

namespace uam::memory
{
	inline constexpr const char* kFailuresAi = "Failures/AI_Failures";
	inline constexpr const char* kFailuresUser = "Failures/User_Failures";
	inline constexpr const char* kLessonsAi = "Lessons/AI_Lessons";
	inline constexpr const char* kLessonsUser = "Lessons/User_Lessons";

	inline const std::vector<std::string>& SupportedCategories()
	{
		static const std::vector<std::string> kCategories = {
		    kFailuresAi,
		    kFailuresUser,
		    kLessonsAi,
		    kLessonsUser,
		};
		return kCategories;
	}

	inline bool IsSupportedCategory(std::string_view category)
	{
		const std::vector<std::string>& categories = SupportedCategories();
		return uam::ranges::Contains(categories, category);
	}
} // namespace uam::memory
