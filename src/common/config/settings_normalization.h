#pragma once

#include "common/models/app_models.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uam::settings
{
	inline constexpr const char* kFocusThemeId = "focus";
	inline constexpr const char* kLegacyMonoThemeId = "mono";
	inline constexpr const char* kDarkThemeId = "dark";
	inline constexpr const char* kLightThemeId = "light";
	inline constexpr const char* kSystemThemeId = "system";
	inline constexpr const char* kMidnightThemeId = "midnight";
	inline constexpr const char* kPaperThemeId = "paper";
	inline constexpr const char* kDuskThemeId = "dusk";
	inline constexpr const char* kAuroraThemeId = "aurora";
	inline constexpr const char* kContrastThemeId = "contrast";
	inline constexpr auto kThemeIds = std::to_array<std::string_view>({
	    kFocusThemeId,
	    kDarkThemeId,
	    kLightThemeId,
	    kSystemThemeId,
	    kMidnightThemeId,
	    kPaperThemeId,
	    kDuskThemeId,
	    kAuroraThemeId,
	    kContrastThemeId,
	});

	inline constexpr int kMinCliIdleTimeoutSeconds = 30;
	inline constexpr int kMaxCliIdleTimeoutSeconds = 3600;
	inline constexpr int kMinActiveTurnInactivityTimeoutSeconds = 60;
	inline constexpr int kMaxActiveTurnInactivityTimeoutSeconds = 86400;
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
	inline constexpr int kDefaultGoalMaxLoopIterations = 200;
	inline constexpr int kMinGoalMaxLoopIterations = 1;
	inline constexpr int kMaxGoalMaxLoopIterations = 200;
	inline constexpr int kDefaultAcpSetupInactivityTimeoutSeconds = 600;
	inline constexpr int kMinAcpSetupInactivityTimeoutSeconds = 60;
	inline constexpr int kMaxAcpSetupInactivityTimeoutSeconds = 3600;
	inline constexpr int kDefaultAcpTurnOutputLimitMiB = 1024;
	inline constexpr int kMinAcpTurnOutputLimitMiB = 256;
	inline constexpr int kMaxAcpTurnOutputLimitMiB = 4096;
	inline constexpr std::size_t kMaxFavoriteUamAgents = 64;
	inline constexpr auto kUamAgentCycleShortcuts = std::to_array<std::string_view>({
	    "shift+tab",
	    "control+shift+tab",
	    "alt+shift+tab",
	    "meta+shift+tab",
	    "disabled",
	});

	inline bool IsUamAgentId(std::string_view value)
	{
		if (value.empty() || value.size() > 64 || value.front() == '-' || value.back() == '-') return false;
		return std::ranges::all_of(value, [](unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
		});
	}

	inline bool IsUamAgentCycleShortcut(std::string_view value)
	{
		return uam::ranges::Contains(kUamAgentCycleShortcuts, value);
	}

	inline void NormalizeUamAgentPreferences(AppSettings& settings)
	{
		std::vector<std::string> normalized;
		normalized.reserve(std::min(settings.favorite_uam_agent_ids.size(), kMaxFavoriteUamAgents));
		std::unordered_set<std::string> seen;
		for (const std::string& raw_id : settings.favorite_uam_agent_ids)
		{
			const std::string id = uam::strings::TrimAndLowerAscii(raw_id);
			if (id == "build" || id == "plan" || !IsUamAgentId(id) ||
			    !seen.insert(id).second)
			{
				continue;
			}
			normalized.push_back(id);
			if (normalized.size() == kMaxFavoriteUamAgents) break;
		}
		settings.favorite_uam_agent_ids = std::move(normalized);
		settings.uam_agent_cycle_shortcut = uam::strings::TrimAndLowerAscii(settings.uam_agent_cycle_shortcut);
		if (!IsUamAgentCycleShortcut(settings.uam_agent_cycle_shortcut))
		{
			settings.uam_agent_cycle_shortcut = "shift+tab";
		}
	}

	inline bool IsThemeId(std::string_view value)
	{
		return uam::ranges::Contains(kThemeIds, value);
	}

	inline bool IsCustomThemeId(std::string_view value)
	{
		constexpr std::string_view prefix = "custom:";
		if (!value.starts_with(prefix) || value.size() <= prefix.size() || value.size() > prefix.size() + 48)
		{
			return false;
		}
		return std::ranges::all_of(value.substr(prefix.size()), [](unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
		});
	}

	inline std::string NormalizeThemeId(std::string_view value)
	{
		const std::string normalized = uam::strings::TrimAndLowerAscii(value);
		if (normalized == kLegacyMonoThemeId)
		{
			return kFocusThemeId;
		}
		return IsThemeId(normalized) || IsCustomThemeId(normalized) ? normalized : std::string(kFocusThemeId);
	}

	inline void ClampWindowSettings(AppSettings& settings)
	{
		settings.ui_scale_multiplier = std::clamp(settings.ui_scale_multiplier, kMinUiScaleMultiplier, kMaxUiScaleMultiplier);
		settings.sidebar_width = std::clamp(settings.sidebar_width, kMinSidebarWidth, kMaxSidebarWidth);
		settings.window_width = std::clamp(settings.window_width, kMinWindowWidth, kMaxWindowWidth);
		settings.window_height = std::clamp(settings.window_height, kMinWindowHeight, kMaxWindowHeight);
	}

	inline void ClampRuntimeTimeoutSettings(AppSettings& settings)
	{
		settings.cli_idle_timeout_seconds = std::clamp(settings.cli_idle_timeout_seconds, kMinCliIdleTimeoutSeconds, kMaxCliIdleTimeoutSeconds);
		settings.active_turn_inactivity_timeout_seconds = std::clamp(settings.active_turn_inactivity_timeout_seconds, kMinActiveTurnInactivityTimeoutSeconds, kMaxActiveTurnInactivityTimeoutSeconds);
		settings.acp_setup_inactivity_timeout_seconds = settings.acp_setup_inactivity_timeout_seconds <= 0
		                                                    ? kDefaultAcpSetupInactivityTimeoutSeconds
		                                                    : std::clamp(settings.acp_setup_inactivity_timeout_seconds, kMinAcpSetupInactivityTimeoutSeconds, kMaxAcpSetupInactivityTimeoutSeconds);
	}

	inline void ClampMemorySettings(AppSettings& settings)
	{
		settings.memory_idle_delay_seconds = std::clamp(settings.memory_idle_delay_seconds, kMinMemoryIdleDelaySeconds, kMaxMemoryIdleDelaySeconds);
		settings.memory_recall_budget_bytes = std::clamp(settings.memory_recall_budget_bytes, kMinMemoryRecallBudgetBytes, kMaxMemoryRecallBudgetBytes);
	}

	inline void ClampGoalSettings(AppSettings& settings)
	{
		settings.goal_max_loop_iterations = settings.goal_max_loop_iterations <= 0
		                                        ? kDefaultGoalMaxLoopIterations
		                                        : std::clamp(settings.goal_max_loop_iterations,
		                                                     kMinGoalMaxLoopIterations,
		                                                     kMaxGoalMaxLoopIterations);
	}

	inline void ClampAcpOutputSettings(AppSettings& settings)
	{
		settings.acp_turn_output_limit_mib = settings.acp_turn_output_limit_mib <= 0
		                                         ? kDefaultAcpTurnOutputLimitMiB
		                                         : std::clamp(settings.acp_turn_output_limit_mib,
		                                                      kMinAcpTurnOutputLimitMiB,
		                                                      kMaxAcpTurnOutputLimitMiB);
	}
} // namespace uam::settings
