#include "common/runtime/acp/acp_session_internal.h"

#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/nlohmann_json_utils.h"

#include <sstream>
#include <string>

namespace uam::acp_detail
{

std::string AcpMessageMethodForDiagnostics(const nlohmann::json& message)
{
	return JsonDiagnosticStringValue(message, "method");
}

std::string AcpMessageRequestIdForDiagnostics(const nlohmann::json& message)
{
	return JsonRpcIdToDiagnosticString(JsonRpcIdOrNull(message));
}

std::string PromptLengthDetail(const nlohmann::json& params)
{
	const nlohmann::json prompt = JsonArrayValue(params, "prompt");
	const std::size_t prompt_chars = uam::acp_content::SumTextFieldSizes(prompt);
	std::ostringstream out;
	out << "sessionId=" << JsonDiagnosticStringValue(params, "sessionId") << ", prompt_chars=" << prompt_chars;
	return out.str();
}

std::string AcpMessageDetailForDiagnostics(const nlohmann::json& message)
{
	if (!message.is_object())
	{
		return "payload is not an object";
	}

	const std::string method = JsonDiagnosticStringValue(message, "method");
	const nlohmann::json params = JsonObjectValue(message, "params");
	if (method == uam::acp_methods::kInitialize)
	{
		return "protocolVersion=" + std::to_string(JsonIntegerValueOr(params, "protocolVersion", 0));
	}
	if (method == uam::acp_methods::kSessionNew)
	{
		return "cwd=" + JsonDiagnosticStringValue(params, "cwd");
	}
	if (method == uam::acp_methods::kSessionLoad)
	{
		return "sessionId=" + JsonDiagnosticStringValue(params, "sessionId") + ", cwd=" + JsonDiagnosticStringValue(params, "cwd");
	}
	if (method == uam::acp_methods::kSessionPrompt)
	{
		return PromptLengthDetail(params);
	}
	if (method == uam::acp_methods::kTurnStart)
	{
		const nlohmann::json input = JsonArrayValue(params, "input");
		const std::size_t input_chars = uam::acp_content::SumTextFieldSizes(input);
		std::ostringstream out;
		out << "threadId=" << JsonDiagnosticStringValue(params, "threadId") << ", input_chars=" << input_chars;
		const std::string model = JsonDiagnosticStringValue(params, "model");
		if (!model.empty())
		{
			out << ", model=" << model;
		}
		const nlohmann::json collaboration_mode = JsonObjectValue(params, "collaborationMode");
		if (collaboration_mode.is_object())
		{
			out << ", collaborationMode=" << JsonDiagnosticStringValue(collaboration_mode, "mode");
			const nlohmann::json settings = JsonObjectValue(collaboration_mode, "settings");
			const std::string collaboration_model = JsonDiagnosticStringValue(settings, "model");
			if (!collaboration_model.empty())
			{
				out << ", collaborationModel=" << collaboration_model;
			}
		}
		return out.str();
	}
	if (method == uam::acp_methods::kSessionCancel)
	{
		return "sessionId=" + JsonDiagnosticStringValue(params, "sessionId");
	}
	if (method == uam::acp_methods::kSessionSetMode)
	{
		return "sessionId=" + JsonDiagnosticStringValue(params, "sessionId") + ", modeId=" + JsonDiagnosticStringValue(params, "modeId");
	}
	if (method == uam::acp_methods::kSessionSetModel)
	{
		return "sessionId=" + JsonDiagnosticStringValue(params, "sessionId") + ", modelId=" + JsonDiagnosticStringValue(params, "modelId");
	}
	if (const nlohmann::json* error = uam::nlohmann_json::FindObjectField(message, "error"); error != nullptr)
	{
		std::ostringstream out;
		out << "error_code=" << JsonIntegerValueOr(*error, "code", 0) << ", error_message=" << JsonDiagnosticStringValue(*error, "message");
		return out.str();
	}
	if (const nlohmann::json* result = uam::nlohmann_json::FindField(message, "result"); result != nullptr)
	{
		return "response_result=" + CapDiagnosticString(result->dump(), kMaxAcpDiagnosticFieldBytes);
	}
	return "";
}

} // namespace uam::acp_detail
