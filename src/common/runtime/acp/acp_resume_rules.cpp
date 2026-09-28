#include "common/runtime/acp/acp_session_internal.h"

namespace uam::acp_detail
{

std::string ResolvedAcpResumeIdForChat(const AppState& app, const ChatSession& chat)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(chat.provider_id);
	const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
	if (resolved != app.resolved_native_sessions_by_chat_id.end())
	{
		// Validate cached identity without copying the transcript.
		ChatSession resolved_chat;
		resolved_chat.provider_id = chat.provider_id;
		resolved_chat.native_session_id = resolved->second;
		const std::string resolved_session_id = runtime.OnAcpValidateResumeId(resolved_chat);
		if (!resolved_session_id.empty())
		{
			return resolved_session_id;
		}
	}

	return runtime.OnAcpValidateResumeId(chat);
}

} // namespace uam::acp_detail
