#pragma once

#include "common/models/app_models.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace uam::settings
{
	inline constexpr const char* kDarkThemeId = "dark";
	inline constexpr const char* kLightThemeId = "light";
	inline constexpr const char* kSystemThemeId = "system";
	inline constexpr auto kThemeIds = std::to_array<std::string_view>({
	    kDarkThemeId,
	    kLightThemeId,
	    kSystemThemeId,
	});

	inline constexpr int kMinCliIdleTimeoutSeconds = 30;
	inline constexpr int kMaxCliIdleTimeoutSeconds = 3600;
	inline constexpr float kMinUiScaleMultiplier = 0.85f;
	inline constexpr float kMaxUiScaleMultiplier = 1.75f;
	inline constexpr float kMinSidebarWidth = 220.0f;
	inline constexpr float kMaxSidebarWidth = 600.0f;
	inline constexpr int kMinWindowWidth = 960;
	inline constexpr int kMaxWindowWidth = 8192;
	inline constexpr int kMinWindowHeight = 620;
	inline constexpr int kMaxWindowHeight = 8192;
	inline constexpr int kMinMemoryIdleDelaySeconds = 30;
	inline constexpr int kMaxMemoryIdleDelaySeconds = 3600;
	inline constexpr int kMinMemoryRecallBudgetBytes = 512;
	inline constexpr int kMaxMemoryRecallBudgetBytes = 8192;

	inline bool IsThemeId(std::string_view value)
	{
		return uam::ranges::Contains(kThemeIds, value);
	}

	inline std::string NormalizeThemeId(std::string_view value)
	{
		const std::string normalized = uam::strings::TrimAndLowerAscii(value);
		return IsThemeId(normalized) ? normalized : std::string(kDarkThemeId);
	}

	inline void ClampWindowSettings(AppSettings& settings)
	{
		settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, kMinUiScaleMultiplier, kMaxUiScaleMultiplier);
		settings.sidebar_width = std::clamp(settings.sidebar_width, kMinSidebarWidth, kMaxSidebarWidth);
		settings.window_width = std::clamp(settings.window_width, kMinWindowWidth, kMaxWindowWidth);
		settings.window_height = std::clamp(settings.window_height, kMinWindowHeight, kMaxWindowHeight);
	}

	inline void ClampMemorySettings(AppSettings& settings)
	{
		settings.memory_idle_delay_seconds = std::clamp(settings.memory_idle_delay_seconds, kMinMemoryIdleDelaySeconds, kMaxMemoryIdleDelaySeconds);
		settings.memory_recall_budget_bytes = std::clamp(settings.memory_recall_budget_bytes, kMinMemoryRecallBudgetBytes, kMaxMemoryRecallBudgetBytes);
	}
} // namespace uam::settings
