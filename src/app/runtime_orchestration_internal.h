#pragma once

// Shared inline helpers used by both pending_call_service.cpp and
// runtime_orchestration_services.cpp.  Include only from those two TUs.

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_branching.h"
#include "common/models/app_models.h"
#include "common/provider/provider_profile.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace uam::runtime_orch_impl
{
	// Rebuild app.chats from a normalised list and preserve resolved native session ids.
	inline void ReplaceAppChatsWithNormalized(uam::AppState& app, std::vector<ChatSession> chats)
	{
		app.chats = ChatDomainService().DeduplicateChatsById(std::move(chats));
		ChatBranching::Normalize(app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(app);

		std::unordered_map<std::string, std::string> next_resolved_native_sessions_by_chat_id;
		next_resolved_native_sessions_by_chat_id.reserve(app.chats.size());

		for (const ChatSession& chat : app.chats)
		{
			std::string resolved_native_session_id;
			const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
			if (resolved != app.resolved_native_sessions_by_chat_id.end())
			{
				resolved_native_session_id = uam::strings::Trim(resolved->second);
			}

			if (resolved_native_session_id.empty())
			{
				resolved_native_session_id = NativeSessionLinkService().RealNativeSessionId(chat);
			}

			if (!resolved_native_session_id.empty())
			{
				next_resolved_native_sessions_by_chat_id[chat.id] = std::move(resolved_native_session_id);
			}
		}

		app.resolved_native_sessions_by_chat_id = std::move(next_resolved_native_sessions_by_chat_id);
	}

	// Native session ids that are currently claimed by resolved chats or active CLI terminals.
	inline std::unordered_set<std::string> ClaimedNativeSessionIds(const uam::AppState& app)
	{
		std::unordered_set<std::string> claimed_session_ids;

		for (const auto& resolved : app.resolved_native_sessions_by_chat_id)
		{
			if (!resolved.second.empty())
			{
				claimed_session_ids.insert(resolved.second);
			}
		}

		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr)
			{
				continue;
			}

			const std::string attached_session_id = uam::CliTerminalAttachedSessionId(*terminal);
			if (!attached_session_id.empty())
			{
				claimed_session_ids.insert(attached_session_id);
			}
		}

		return claimed_session_ids;
	}

	// Resolve the provider for a pending call, falling back to the active provider.
	inline const ProviderProfile& ResolvePendingCallProviderOrDefault(const uam::AppState& app, const PendingRuntimeCall& call)
	{
		if (!call.provider_id_snapshot.empty())
		{
			if (const ProviderProfile* profile = ProviderProfileStore::FindById(app.provider_profiles, call.provider_id_snapshot); profile != nullptr)
			{
				return *profile;
			}
		}

		return ProviderResolutionService().ActiveProviderOrDefault(app);
	}
} // namespace uam::runtime_orch_impl
