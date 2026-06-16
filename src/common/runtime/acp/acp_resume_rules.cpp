#include "common/runtime/acp/acp_session_internal.h"

#include "app/native_session_link_service.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/utils/string_utils.h"

namespace uam::acp_detail
{

std::string ValidCodexResumeId(const ChatSession& chat)
{
	return uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
}

std::string ValidGeminiResumeId(const ChatSession& chat)
{
	return NativeSessionLinkService().RealNativeSessionId(chat);
}

std::string ValidGenericAcpResumeId(const ChatSession& chat)
{
	return NativeSessionLinkService().RealNativeSessionId(chat);
}

std::string ResolvedAcpResumeIdForChat(const AppState& app, const ChatSession& chat)
{
	const auto resolved = app.resolved_native_sessions_by_chat_id.find(chat.id);
	if (resolved != app.resolved_native_sessions_by_chat_id.end())
	{
		const std::string resolved_session_id = uam::strings::Trim(resolved->second);
		if (!resolved_session_id.empty())
		{
			return resolved_session_id;
		}
	}

	return uam::strings::Trim(chat.native_session_id);
}

} // namespace uam::acp_detail
