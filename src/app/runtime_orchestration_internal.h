#pragma once

// Shared helpers for native history imports and chat-list replacement.

#include "app/chat_domain_service.h"
#include "app/native_session_link_service.h"
#include "common/chat/chat_branching.h"
#include "common/models/app_models.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/state/app_state.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace uam::runtime_orch_impl
{
	inline bool ChatNeedsTranscriptPreserved(const uam::AppState& app, const ChatSession& chat)
	{
		return app.pending_chat_save_at_by_chat_id.contains(chat.id) ||
		       std::ranges::any_of(app.acp_sessions, [&chat](const std::unique_ptr<uam::AcpSessionState>& session)
		       { return session != nullptr && session->chat_id == chat.id && (uam::AcpSessionHasActiveTurn(*session) || session->turn_checkpoint_commit_pending); });
	}

	// Rebuild the list without replacing live turns or unsaved chats with disk snapshots.
	inline void ReplaceAppChatsWithNormalized(uam::AppState& app, std::vector<ChatSession> chats)
	{
		std::unordered_map<std::string, ChatSession> live_chats;
		for (ChatSession& chat : app.chats)
		{
			if (ChatNeedsTranscriptPreserved(app, chat))
				live_chats.emplace(chat.id, std::move(chat));
		}
		for (ChatSession& chat : chats)
		{
			const std::unordered_map<std::string, ChatSession>::iterator live = live_chats.find(chat.id);
			if (live == live_chats.end()) continue;
			chat = std::move(live->second);
			live_chats.erase(live);
		}
		for (std::pair<const std::string, ChatSession>& live : live_chats)
			chats.push_back(std::move(live.second));

		app.chats = ChatDomainService().DeduplicateChatsById(std::move(chats));
		ChatBranching::Normalize(app.chats);
		ChatDomainService().NormalizeChatFolderAssignments(app);
		std::ranges::stable_partition(app.chats, [](const ChatSession& chat) { return chat.agent_run_id.empty(); });

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

} // namespace uam::runtime_orch_impl
