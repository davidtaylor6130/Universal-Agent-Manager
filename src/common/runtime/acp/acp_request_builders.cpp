#include "common/runtime/acp/acp_session_internal.h"

#include "common/config/approval_modes.h"
#include "common/provider/codex/codex_options.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

nlohmann::json BuildInitializeRequest(int request_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kInitialize,
	                                  {
	                                      {"protocolVersion", 1},
	                                      {"clientCapabilities", nlohmann::json::object()},
	                                      {"clientInfo", uam::acp_request_defaults::ClientInfo()},
	                                  });
}

nlohmann::json BuildCodexInitializeRequest(int request_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kInitialize,
	                                  {
	                                      {"clientInfo", uam::acp_request_defaults::ClientInfo()},
	                                      {"capabilities",
	                                       {
	                                           {"experimentalApi", true},
	                                       }},
	                                  });
}

nlohmann::json BuildCodexInitializedNotification()
{
	return uam::acp_json_rpc::Notification(uam::acp_methods::kInitialized, nullptr);
}

nlohmann::json BuildCodexModelListRequest(int request_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kModelList, nlohmann::json::object());
}

nlohmann::json BuildNewSessionRequest(int request_id, const std::string& cwd)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionNew,
	                                  {
	                                      {"cwd", cwd},
	                                      {"mcpServers", nlohmann::json::array()},
	                                  });
}

nlohmann::json BuildLoadSessionRequest(int request_id, const std::string& session_id, const std::string& cwd)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionLoad,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"cwd", cwd},
	                                      {"mcpServers", nlohmann::json::array()},
	                                  });
}

nlohmann::json BuildCodexThreadStartRequest(int request_id, const ChatSession& chat, const std::string& cwd)
{
	nlohmann::json params = uam::acp_request_defaults::CodexThreadStartParams(cwd);

	const std::string model_id = uam::strings::Trim(chat.model_id);
	if (!model_id.empty())
	{
		params["model"] = model_id;
	}

	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kThreadStart, std::move(params));
}

nlohmann::json BuildCodexThreadResumeRequest(int request_id, const ChatSession& chat, const std::string& cwd)
{
	nlohmann::json params = uam::acp_request_defaults::CodexThreadResumeParams(chat.native_session_id, cwd);

	const std::string model_id = uam::strings::Trim(chat.model_id);
	if (!model_id.empty())
	{
		params["model"] = model_id;
	}

	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kThreadResume, std::move(params));
}

nlohmann::json BuildGeminiSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd, bool load_session_supported)
{
	const std::string resume_id = ValidGeminiResumeId(chat);
	if (!resume_id.empty() && load_session_supported)
	{
		return BuildLoadSessionRequest(request_id, resume_id, cwd);
	}

	return BuildNewSessionRequest(request_id, cwd);
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
			const bool before_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
			const std::size_t end_pos = pos + needle.size();
			const bool after_ok = (end_pos >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[end_pos]));
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

bool GeminiErrorLooksLikeInvalidSessionId(const std::string& error_message, const std::string& error_data)
{
	const std::string text = error_message + "\n" + error_data;
	return TextContainsAnyCaseInsensitive(text, {
	                                           "invalid session identifier",
	                                           "use --list-sessions",
	                                       });
}

nlohmann::json BuildCodexSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd)
{
	const std::string resume_id = ValidCodexResumeId(chat);
	if (resume_id.empty())
	{
		return BuildCodexThreadStartRequest(request_id, chat, cwd);
	}

	ChatSession resume_chat = chat;
	resume_chat.native_session_id = resume_id;
	return BuildCodexThreadResumeRequest(request_id, resume_chat, cwd);
}

nlohmann::json BuildPromptRequest(int request_id, const std::string& session_id, const std::string& text)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kSessionPrompt,
	                                  {
	                                      {"sessionId", session_id},
	                                      {"prompt", nlohmann::json::array({uam::acp_content::TextPart(text)})},
	                                  });
}

nlohmann::json BuildCodexTurnStartRequest(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id)
{
	nlohmann::json params = {
	    {"threadId", thread_id},
	    {"input", nlohmann::json::array({uam::acp_content::CodexTextInputPart(text)})},
	};

	const std::string model_id = uam::strings::Trim(chat.model_id);
	const std::string collaboration_model_id = model_id.empty() ? uam::strings::Trim(active_model_id) : model_id;
	const std::string reasoning_effort = uam::codex::NormalizeReasoningEffort(chat.reasoning_effort);
	const std::string service_tier = uam::codex::NormalizeServiceTier(chat.service_tier);
	if (!model_id.empty())
	{
		params["model"] = model_id;
	}
	if (!reasoning_effort.empty())
	{
		params["effort"] = reasoning_effort;
	}
	if (!service_tier.empty())
	{
		params["serviceTier"] = service_tier;
	}

	const std::string app_mode_id = uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode);
	const std::string requested_mode_id = app_mode_id == uam::approval_modes::kPlanApprovalMode ? uam::approval_modes::kPlanApprovalMode : uam::approval_modes::kDefaultApprovalMode;
	if (!collaboration_model_id.empty())
	{
		nlohmann::json settings = {
		    {"model", collaboration_model_id},
		    {"reasoning_effort", uam::nlohmann_json::StringOrNull(reasoning_effort)},
		    {"developer_instructions", nullptr},
		};
		params["collaborationMode"] = {
		    {"mode", requested_mode_id},
		    {"settings", std::move(settings)},
		};
	}

	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kTurnStart, std::move(params));
}

nlohmann::json BuildCancelNotification(const std::string& session_id)
{
	return uam::acp_json_rpc::Notification(uam::acp_methods::kSessionCancel, {
	                                                                             {"sessionId", session_id},
	                                                                         });
}

nlohmann::json BuildCodexTurnInterruptRequest(int request_id, const std::string& thread_id, const std::string& turn_id)
{
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kTurnInterrupt,
	                                  {
	                                      {"threadId", thread_id},
	                                      {"turnId", turn_id},
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
	return uam::approval_modes::AppApprovalModeFromProviderModeId(uam::strings::TrimAsciiView(mode_id));
}

std::string ProviderApprovalModeId(const AcpSessionState& session, const std::string& mode_id)
{
	return ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpMapApprovalModeId(mode_id);
}

} // namespace uam::acp_detail
