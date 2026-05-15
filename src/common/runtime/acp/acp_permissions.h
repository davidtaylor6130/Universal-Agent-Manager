#pragma once

#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <array>
#include <string>
#include <string_view>

namespace uam::acp_permissions
{
	inline constexpr const char* kCodexCommandRequestKind = "codex-command";
	inline constexpr const char* kCodexFileRequestKind = "codex-file";
	inline constexpr const char* kCodexPermissionsRequestKind = "codex-permissions";
	inline constexpr const char* kPermissionsToolKind = "permissions";

	inline constexpr const char* kAcceptDecision = "accept";
	inline constexpr const char* kAcceptForSessionDecision = "acceptForSession";
	inline constexpr const char* kDeclineDecision = "decline";
	inline constexpr const char* kCancelDecision = "cancel";
	inline constexpr const char* kCancelledOptionId = "cancelled";
	inline constexpr const char* kDecisionOptionKind = "decision";
	inline constexpr const char* kCancelOptionKind = "cancel";
	inline constexpr const char* kPermissionsField = "permissions";
	inline constexpr const char* kScopeField = "scope";
	inline constexpr const char* kSessionScope = "session";
	inline constexpr const char* kOutcomeField = "outcome";
	inline constexpr const char* kCancelledOutcome = "cancelled";
	inline constexpr const char* kSelectedOutcome = "selected";
	inline constexpr const char* kOptionIdField = "optionId";

	inline constexpr auto kCodexDecisionPermissionKinds = std::to_array<std::string_view>({
	    kCodexCommandRequestKind,
	    kCodexFileRequestKind,
	});

	inline constexpr auto kDenyDecisionOptionIds = std::to_array<std::string_view>({
	    kDeclineDecision,
	    kCancelledOptionId,
	});

	inline bool IsCodexDecisionPermissionKind(std::string_view kind)
	{
		return uam::ranges::Contains(kCodexDecisionPermissionKinds, uam::strings::TrimAsciiView(kind));
	}

	inline bool IsCodexDecisionPermissionKind(const char* kind)
	{
		return IsCodexDecisionPermissionKind(uam::strings::ViewOrEmpty(kind));
	}

	inline bool IsDenyDecision(std::string_view option_id, bool cancelled)
	{
		return cancelled || uam::ranges::Contains(kDenyDecisionOptionIds, uam::strings::TrimAsciiView(option_id));
	}

	inline bool IsDenyDecision(const char* option_id, bool cancelled)
	{
		return IsDenyDecision(uam::strings::ViewOrEmpty(option_id), cancelled);
	}

	inline std::string CodexDecisionForOption(std::string_view option_id, bool cancelled)
	{
		option_id = uam::strings::TrimAsciiView(option_id);
		if (cancelled)
		{
			return kCancelDecision;
		}
		if (IsDenyDecision(option_id, false))
		{
			return kDeclineDecision;
		}
		return uam::strings::NonEmptyOrFallback(option_id, kAcceptDecision);
	}

	inline std::string CodexDecisionForOption(const char* option_id, bool cancelled)
	{
		return CodexDecisionForOption(uam::strings::ViewOrEmpty(option_id), cancelled);
	}

	inline std::string CodexDecisionLabel(std::string_view option_id)
	{
		option_id = uam::strings::TrimAsciiView(option_id);
		if (option_id == kAcceptForSessionDecision)
		{
			return "Allow for session";
		}
		if (option_id == kAcceptDecision)
		{
			return "Allow";
		}
		if (option_id == kDeclineDecision)
		{
			return "Deny";
		}
		return std::string(option_id);
	}

	inline std::string CodexDecisionLabel(const char* option_id)
	{
		return CodexDecisionLabel(uam::strings::ViewOrEmpty(option_id));
	}
} // namespace uam::acp_permissions
