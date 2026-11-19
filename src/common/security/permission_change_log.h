#pragma once

#include "common/config/approval_modes.h"
#include "common/config/build_features.h"
#include "common/models/app_models.h"
#include "common/security/command_safety.h"
#include "common/utils/diagnostic_log.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

/// <summary>Diagnostic record of changes to a chat's approval mode or command safety tier.</summary>
namespace uam::permission_log
{
	inline constexpr std::string_view kSourceUser = "user";
#if UAM_ENABLE_MOBILE_COMPANION
	inline constexpr std::string_view kSourceCompanion = "companion";
#endif
	inline constexpr std::string_view kSourceRollback = "rollback";
	inline constexpr std::string_view kSourceSwitch = "switch";
	inline constexpr std::string_view kSourceMigration = "migration";
	inline constexpr std::string_view kSourcePlanApproval = "planApproval";

	/// <summary>Names the mode and permission level in force after a rejected change, for the failure the user sees.</summary>
	inline std::string RollbackNotice(const ChatSession& chat)
	{
		return std::string("The change was not applied; this chat is ") + (chat.approval_mode == uam::approval_modes::kPlanApprovalMode ? "in" : "not in") +
		    " Plan mode and its permission level is " + uam::command_safety::TierDisplayName(chat.command_safety_tier) + ".";
	}

	/// <summary>Bounds a stored identifier; a chat file can hold any string in these fields.</summary>
	inline std::string BoundedValue(std::string_view value)
	{
		constexpr std::size_t kMaxValueBytes = 64;
		std::string bounded;
		for (const char ch : value.substr(0, kMaxValueBytes))
		{
			bounded.push_back(ch == '\n' || ch == '\r' ? ' ' : ch);
		}
		return bounded.empty() ? std::string("(empty)") : bounded;
	}

	/// <summary>Formats one change line. Holds only the chat id, fixed mode names and the source; never prompt text.</summary>
	inline std::string FormatChange(std::string_view chat_id, std::string_view source,
	    std::string_view old_approval_mode, std::string_view new_approval_mode,
	    std::string_view old_tier, std::string_view new_tier)
	{
		std::string line = "[permission] chat=" + BoundedValue(chat_id) + " source=" + std::string(source);
		if (old_tier != new_tier) line += " commandSafetyTier=" + BoundedValue(old_tier) + "->" + BoundedValue(new_tier);
		if (old_approval_mode != new_approval_mode) line += " approvalMode=" + BoundedValue(old_approval_mode) + "->" + BoundedValue(new_approval_mode);
		return line;
	}

	/// <summary>Writes one line when the approval mode or command safety tier differs; writes nothing otherwise.</summary>
	inline void RecordChange(std::string_view chat_id, std::string_view source,
	    std::string_view old_approval_mode, std::string_view new_approval_mode,
	    std::string_view old_tier, std::string_view new_tier)
	{
		if (old_approval_mode == new_approval_mode && old_tier == new_tier) return;
		uam::diagnostics::Write(FormatChange(chat_id, source, old_approval_mode, new_approval_mode, old_tier, new_tier));
	}

	inline void RecordChange(const ChatSession& before, const ChatSession& after, std::string_view source)
	{
		RecordChange(after.id, source, before.approval_mode, after.approval_mode, before.command_safety_tier, after.command_safety_tier);
	}

	/// <summary>Logs a load-time migration once per chat and process; a chat file is parsed for summaries and full loads until it is saved again.</summary>
	inline void RecordMigrationOnce(std::string_view chat_id,
	    std::string_view old_approval_mode, std::string_view new_approval_mode,
	    std::string_view old_tier, std::string_view new_tier)
	{
		if (old_approval_mode == new_approval_mode && old_tier == new_tier) return;
		static std::mutex mutex;
		static std::unordered_set<std::string> logged;
		{
			const std::lock_guard lock(mutex);
			if (!logged.insert(std::string(chat_id)).second) return;
		}
		uam::diagnostics::Write(FormatChange(chat_id, kSourceMigration, old_approval_mode, new_approval_mode, old_tier, new_tier));
	}
} // namespace uam::permission_log
