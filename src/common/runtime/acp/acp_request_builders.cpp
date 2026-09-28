#include "common/runtime/acp/acp_session_internal.h"

#include "common/config/approval_modes.h"
#include "computer_use/computer_use_mcp_config.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/string_utils.h"

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

nlohmann::json BuildNewSessionRequest(int request_id, const std::string& cwd, const ChatSession* chat)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionNew,
	                                  {
	                                      {"cwd", cwd},
	                                      {"mcpServers", chat == nullptr ? nlohmann::json::array() : uam::computer_use::AcpMcpServers(*chat)},
	                                  });
}

nlohmann::json BuildLoadSessionRequest(int request_id, const std::string& session_id, const std::string& cwd, const ChatSession* chat)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionLoad,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"cwd", cwd},
	                                      {"mcpServers", chat == nullptr ? nlohmann::json::array() : uam::computer_use::AcpMcpServers(*chat)},
	                                  });
}

nlohmann::json BuildResumeSessionRequest(int request_id, const std::string& session_id,
	                                     const std::string& cwd, const ChatSession* chat)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionResume,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"cwd", cwd},
	                                      {"mcpServers", chat == nullptr ? nlohmann::json::array() : uam::computer_use::AcpMcpServers(*chat)},
	                                  });
}

bool TextContainsAnyCaseInsensitive(std::string_view text, std::initializer_list<std::string_view> needles)
{
	return uam::strings::ContainsAnyCaseInsensitive(text, needles);
}

bool WordMatchesAnyCaseInsensitive(std::string_view text, std::initializer_list<std::string_view> words)
{
	if (text.empty())
		return false;

	std::string lower_text(text.size(), '\0');
	for (std::size_t i = 0; i < text.size(); ++i)
	{
		lower_text[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
	}

	for (const auto& needle : words)
	{
		if (needle.empty())
			continue;
		std::size_t pos = 0;
		while ((pos = lower_text.find(needle, pos)) != std::string_view::npos)
		{
			const bool before_ok = (pos == 0) || (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_');
			const std::size_t end_pos = pos + needle.size();
			const bool after_ok = (end_pos >= text.size()) || (!std::isalnum(static_cast<unsigned char>(text[end_pos])) && text[end_pos] != '_');
			if (before_ok && after_ok)
				return true;
			pos += 1;
		}
	}
	return false;
}

bool AcpSessionCanSendQueuedPrompt(const AcpSessionState& session)
{
	if (!session.running ||
	    !session.session_ready ||
	    !session.processing ||
	    uam::AcpSessionIsWaitingForInput(session) ||
	    session.prompt_request_id != 0 ||
	    session.queued_prompt.empty())
	{
		return false;
	}

	return ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpCanSendPromptWithoutSessionId() || !session.session_id.empty();
}

nlohmann::json BuildPromptRequest(int request_id, const std::string& session_id, const std::string& text, const std::string& reasoning_effort)
{
	nlohmann::json params = {
	    {"sessionId", session_id},
	    {"prompt", nlohmann::json::array({uam::acp_content::TextPart(text)})},
	};
	if (!reasoning_effort.empty())
	{
		params["reasoningEffort"] = reasoning_effort;
	}
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionPrompt, std::move(params));
}

nlohmann::json BuildCancelNotification(const std::string& session_id)
{
	return uam::acp_json_rpc::Notification(uam::acp_methods::kSessionCancel, {
	                                                                             {"sessionId", session_id},
	                                                                         });
}

nlohmann::json BuildSetConfigOptionRequest(int request_id, const std::string& session_id, const std::string& config_id, const std::string& value)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionSetConfigOption,
		                              {
		                                  {"sessionId", session_id},
		                                  {"configId", config_id},
		                                  {"value", value},
		                              });
}

nlohmann::json BuildSetModeRequest(int request_id, const std::string& session_id, const std::string& mode_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionSetMode,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"modeId", mode_id},
	                                  });
}

nlohmann::json BuildSetModelRequest(int request_id, const std::string& session_id, const std::string& model_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionSetModel,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"modelId", model_id},
	                                  });
}

std::string AppApprovalModeId(std::string_view mode_id)
{
	const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
	// Keep hidden Copilot autopilot state distinct so the prompt path forces it
	// back to the app's safe default agent mode before sending user input.
	if (normalized == uam::approval_modes::kAcpAutopilotMode)
	{
		return std::string(normalized);
	}
	return uam::approval_modes::AppApprovalModeFromProviderModeId(normalized);
}

std::string ProviderApprovalModeId(const AcpSessionState& session, const std::string& mode_id)
{
	return ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpMapApprovalModeId(mode_id);
}

} // namespace uam::acp_detail
