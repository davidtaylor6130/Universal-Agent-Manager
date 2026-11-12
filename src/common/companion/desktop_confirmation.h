#pragma once

#include "common/config/approval_modes.h"
#include "common/security/command_safety.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>

namespace uam::companion
{
	/// The chat fields a companion request may loosen; absent when the request names no known chat.
	struct ConfirmationChatState
	{
		std::string command_safety_tier;
		std::string approval_mode;
		std::string uam_agent_id;
		bool uam_control_enabled = false;
	};

	/// What a companion request would loosen, or empty when it may apply without desktop confirmation.
	/// Invalid values stay ungated: the handler rejects them without changing anything.
	inline std::string LooseningRequest(std::string_view action, const nlohmann::json& payload, const ConfirmationChatState* chat)
	{
		if (action == "setProviderChatDefaults") return "Change provider chat defaults";
		if (!payload.is_object()) return {};
		// A new chat may only start at Default or Plan with default safety; anything looser is the desktop's call.
		if (action == "createSession")
		{
			const auto defaults = payload.find("defaults");
			if (defaults == payload.end() || !defaults->is_object()) return {};
			const auto field = [&](const char* key) { return defaults->contains(key) && (*defaults)[key].is_string() ? uam::strings::Trim((*defaults)[key].get<std::string>()) : std::string{}; };
			const std::string tier = field("commandSafetyTier");
			if (!tier.empty() && command_safety::ParseTier(tier) != command_safety::Tier::Off)
				return "Create a chat with permission mode " + command_safety::TierDisplayName(tier);
			const std::string mode = approval_modes::NormalizeIncomingApprovalModeId(field("approvalMode"));
			if (!field("approvalMode").empty() && mode != approval_modes::kDefaultApprovalMode && mode != approval_modes::kPlanApprovalMode)
				return "Create a chat with approval mode " + mode;
			return {};
		}
		// MCP servers add tools every agent can call: only a plain disable applies directly.
		if (action == "setProviderMcpConfiguration")
		{
			const bool disable_only = payload.contains("enabled") && payload["enabled"] == false && !payload.contains("server");
			const std::string server = payload.contains("serverId") && payload["serverId"].is_string() ? payload["serverId"].get<std::string>() : std::string("an MCP server");
			return disable_only ? std::string{} : "Turn on or change " + server;
		}
		// Provider-native config options can carry the session's permission or sandbox mode.
		if (action == "setAcpConfigOption")
		{
			const std::string id = payload.contains("configId") && payload["configId"].is_string() ? uam::strings::ToLowerAscii(payload["configId"].get<std::string>()) : std::string{};
			for (const char* sensitive : {"mode", "approval", "permission", "sandbox", "autonomy", "yolo"})
				if (id.find(sensitive) != std::string::npos) return "Change the provider option " + id;
			return {};
		}
		if (chat == nullptr) return {};
		const auto text = [&](const char* key) { return payload.contains(key) && payload[key].is_string() ? uam::strings::Trim(payload[key].get<std::string>()) : std::string{}; };
		if (action == "setChatCommandSafetyTier")
		{
			const std::string requested = uam::strings::ToLowerAscii(text("commandSafetyTier"));
			if (requested != "acceptedits" && requested != "aireview" && requested != "yolo") return {};
			if (command_safety::NormalizeTier(requested) == command_safety::NormalizeTier(chat->command_safety_tier)) return {};
			return "Change permission mode to " + command_safety::TierDisplayName(requested);
		}
		if (action == "setChatApprovalMode")
		{
			const std::string requested = approval_modes::NormalizeIncomingApprovalModeId(text("modeId"));
			const std::string current = approval_modes::NormalizeIncomingApprovalModeId(chat->approval_mode);
			if (!approval_modes::IsAgentMode(requested) || requested == approval_modes::kPlanApprovalMode || requested == current) return {};
			return "Leave Plan mode";
		}
		if (action == "setChatUamAgent")
		{
			const std::string requested = uam::strings::NonEmptyOrFallback(text("agentId"), "build");
			if (requested == uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat->uam_agent_id), "build")) return {};
			return "Change UAM agent to " + requested;
		}
		if (action == "setChatUamControlEnabled")
			return payload.contains("enabled") && payload["enabled"] == true && !chat->uam_control_enabled ? "Turn on UAM Control" : std::string{};
		return {};
	}

	/// Companion requests held until the user answers on the desktop. Bounded and short-lived.
	class DesktopConfirmations
	{
	  public:
		using Clock = std::chrono::system_clock;
		static constexpr std::size_t kMaxPending = 8;
		static constexpr std::chrono::minutes kLifetime{5};

		struct Pending
		{
			std::string id, action, chat_id, chat_title, description;
			nlohmann::json payload;
			Clock::time_point expires_at;
		};

		/// Adds a request; false when the queue is full of unexpired requests.
		bool Add(Pending pending, Clock::time_point now)
		{
			Expire(now);
			if (m_pending.size() >= kMaxPending) return false;
			pending.id = "confirm-" + std::to_string(++m_serial);
			pending.expires_at = now + kLifetime;
			m_pending.push_back(std::move(pending));
			return true;
		}

		/// Removes and returns an unexpired request; expired or unknown ids return nothing.
		std::optional<Pending> Take(std::string_view id, Clock::time_point now)
		{
			Expire(now);
			for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
			{
				if (it->id != id) continue;
				Pending taken = std::move(*it);
				m_pending.erase(it);
				return taken;
			}
			return std::nullopt;
		}

		void Expire(Clock::time_point now)
		{
			std::erase_if(m_pending, [now](const Pending& pending) { return pending.expires_at <= now; });
		}

		/// Desktop projection: never includes the stored payload.
		nlohmann::json ToJson() const
		{
			nlohmann::json items = nlohmann::json::array();
			for (const Pending& pending : m_pending)
			{
				items.push_back({{"id", pending.id}, {"chatId", pending.chat_id}, {"chatTitle", pending.chat_title}, {"description", pending.description},
				    {"expiresAt", std::chrono::duration_cast<std::chrono::milliseconds>(pending.expires_at.time_since_epoch()).count()}});
			}
			return items;
		}

	  private:
		std::deque<Pending> m_pending;
		unsigned long long m_serial = 0;
	};
}
