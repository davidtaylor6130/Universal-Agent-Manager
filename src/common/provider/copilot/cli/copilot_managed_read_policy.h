#pragma once

#include "common/provider/provider_runtime.h"

namespace uam::copilot::managed_read
{
bool HandleResult(AppState&, AcpSessionState&, ChatSession&, const std::string& method,
	const std::string& request_id, const nlohmann::json& result);
bool HandleError(AppState&, AcpSessionState&, ChatSession&, const acp_detail::AcpResponseFailureDetails&);
bool Poll(AppState&, AcpSessionState&, ChatSession&);
void ObserveEvent(AppState&, AcpSessionState&, ChatSession&, const nlohmann::json& event);
void ObserveModel(AppState&, AcpSessionState&, const ChatSession&, std::string_view model);
}
