#pragma once

#include "test_harness.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

/// <summary>Pumps private durability work until an admitted prompt reaches its transport.</summary>
inline void AwaitDurablePromptDispatch(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat)
{
	const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (session.prompt_request_id == 0 && session.processing && std::chrono::steady_clock::now() < deadline)
	{
		uam::FlushPendingChatSaves(app);
		uam::acp_detail::SendQueuedPromptIfReady(app, session, chat);
		std::this_thread::yield();
	}
	UAM_ASSERT(session.prompt_request_id != 0);
}
