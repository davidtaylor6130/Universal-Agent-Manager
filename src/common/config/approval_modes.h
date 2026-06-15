#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::approval_modes
{

	inline constexpr const char* kDefaultApprovalMode = "default";
	inline constexpr const char* kAcceptEditsApprovalMode = "acceptEdits";
	inline constexpr const char* kPlanApprovalMode = "plan";
	inline constexpr const char* kLegacyYoloApprovalMode = "yolo";
	inline constexpr const char* kProviderAutoApprovalMode = "auto";
	inline constexpr const char* kProviderAutoEditApprovalMode = "auto_edit";

	inline constexpr auto kAppApprovalModes = std::to_array<std::string_view>({
	    kDefaultApprovalMode,
	    kAcceptEditsApprovalMode,
	    kPlanApprovalMode,
	});

	inline constexpr auto kPersistedProviderDefaultApprovalModes = std::to_array<std::string_view>({
	    kAcceptEditsApprovalMode,
	    kPlanApprovalMode,
	});

	inline constexpr auto kSuppressedProviderApprovalModes = std::to_array<std::string_view>({
	    kLegacyYoloApprovalMode,
	    kProviderAutoApprovalMode,
	});

	inline bool IsAppApprovalMode(std::string_view mode_id)
	{
		return uam::ranges::Contains(kAppApprovalModes, mode_id);
	}

	inline bool IsAppApprovalMode(const char* mode_id)
	{
		return IsAppApprovalMode(uam::strings::ViewOrEmpty(mode_id));
	}

	inline bool IsPersistedProviderDefaultApprovalMode(std::string_view mode_id)
	{
		return uam::ranges::Contains(kPersistedProviderDefaultApprovalModes, mode_id);
	}

	inline std::string NormalizeIncomingApprovalModeId(std::string_view mode_id)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
		if (normalized.empty() || normalized == kLegacyYoloApprovalMode)
		{
			return kDefaultApprovalMode;
		}

		return std::string(normalized);
	}

	inline std::string NormalizePersistedProviderDefaultApprovalMode(std::string_view mode_id)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
		if (IsPersistedProviderDefaultApprovalMode(normalized))
		{
			return std::string(normalized);
		}

		return kDefaultApprovalMode;
	}

	inline std::string AppApprovalModeOrEmpty(std::string_view mode_id)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
		return IsAppApprovalMode(normalized) ? std::string(normalized) : std::string();
	}

	inline bool IsSuppressedProviderApprovalMode(std::string_view mode_id)
	{
		return uam::ranges::Contains(kSuppressedProviderApprovalModes, mode_id);
	}

	inline bool IsSuppressedProviderApprovalMode(const char* mode_id)
	{
		return IsSuppressedProviderApprovalMode(uam::strings::ViewOrEmpty(mode_id));
	}

	inline std::string AppApprovalModeFromProviderModeId(std::string_view mode_id)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
		if (IsSuppressedProviderApprovalMode(normalized))
		{
			return kDefaultApprovalMode;
		}
		if (normalized == kProviderAutoEditApprovalMode)
		{
			return kAcceptEditsApprovalMode;
		}

		return std::string(normalized);
	}

	inline std::string AppApprovalModeFromProviderModeId(const char* mode_id)
	{
		return AppApprovalModeFromProviderModeId(uam::strings::ViewOrEmpty(mode_id));
	}

	inline std::string GeminiProviderApprovalModeFromAppModeId(std::string_view mode_id)
	{
		const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
		return normalized == kAcceptEditsApprovalMode ? std::string(kProviderAutoEditApprovalMode) : std::string(normalized);
	}

	inline std::string GeminiProviderApprovalModeFromAppModeId(const char* mode_id)
	{
		return GeminiProviderApprovalModeFromAppModeId(uam::strings::ViewOrEmpty(mode_id));
	}

} // namespace uam::approval_modes
