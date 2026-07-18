#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "common/config/approval_modes.h"
#include "common/paths/workspace_root.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/runtime/acp/acp_model_json.h"

#include <cstring>
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace uam::acp_detail
{

void HandleAcpRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
{
	(void)app;
	const std::string method = JsonDiagnosticStringValue(message, "method");
	if (method == uam::acp_methods::kSessionUpdate)
	{
		return;
	}

	if (method == uam::acp_methods::kSessionRequestPermission)
	{
		HandlePermissionRequest(app, session, chat, message);
		return;
	}

	if (uam::nlohmann_json::FindField(message, "id") != nullptr)
	{
		const nlohmann::json request_id = JsonRpcIdOrNull(message);
		AppendAcpDiagnostic(session, "request", "unsupported_method", method, JsonRpcIdToStableString(request_id), true, -32601, "UAM ACP client does not implement method: " + method);
		SendJsonRpcError(session, request_id, -32601, "UAM ACP client does not implement method: " + method);
	}
}

std::string PendingRequestSummary(const AcpSessionState& session)
{
	if (session.pending_request_methods.empty())
	{
		return "";
	}

	std::vector<std::string> pending_requests;
	pending_requests.reserve(session.pending_request_methods.size());
	for (const auto& entry : session.pending_request_methods)
	{
		pending_requests.push_back(std::to_string(entry.first) + ":" + entry.second);
	}
	return uam::strings::JoinNonEmpty(pending_requests, ", ");
}

std::string ErrorDataForDiagnostics(const nlohmann::json& error)
{
	const nlohmann::json* data = uam::nlohmann_json::FindField(error, "data");
	if (data == nullptr)
	{
		return "";
	}
	return CapDiagnosticString(data->dump(), kMaxAcpDiagnosticDetailBytes);
}

void UpdateAcpModesFromJson(AcpSessionState& session, const nlohmann::json& modes)
{
	if (!modes.is_object())
	{
		return;
	}

	if (const nlohmann::json* available_modes = uam::nlohmann_json::FindArrayField(modes, "availableModes"); available_modes != nullptr)
	{
		session.available_modes.clear();
		for (const nlohmann::json& mode : *available_modes)
		{
			const std::string provider_mode_id = uam::nlohmann_json::TrimmedStringValue(mode, {"id"});
			if (uam::approval_modes::IsSuppressedProviderApprovalMode(provider_mode_id))
			{
				continue;
			}
			AcpModeState parsed;
			parsed.id = AppApprovalModeId(provider_mode_id);
			parsed.name = uam::nlohmann_json::TrimmedStringValue(mode, {"name"});
			if (parsed.name.empty())
			{
				parsed.name = parsed.id;
			}
			parsed.description = uam::nlohmann_json::TrimmedStringValue(mode, {"description"});
			if (!parsed.id.empty())
			{
				session.available_modes.push_back(std::move(parsed));
			}
		}
	}

	const std::string current_mode_id = uam::nlohmann_json::TrimmedStringValue(modes, {"currentModeId"});
	if (!current_mode_id.empty())
	{
		session.current_mode_id = AppApprovalModeId(current_mode_id);
	}
}

void UpdateAcpModelsFromJson(AcpSessionState& session, const nlohmann::json& models)
{
	if (!models.is_object())
	{
		return;
	}

	if (const nlohmann::json* available_models = uam::nlohmann_json::FindArrayField(models, "availableModels"); available_models != nullptr)
	{
		session.available_models.clear();
		for (const nlohmann::json& model : *available_models)
		{
			if (std::optional<AcpModelState> parsed = uam::acp_models::ParseAcpModelState(model))
			{
				session.available_models.push_back(std::move(*parsed));
			}
		}
	}

	const std::string current_model_id = uam::nlohmann_json::TrimmedStringValue(models, {"currentModelId"});
	if (!current_model_id.empty())
	{
		session.current_model_id = current_model_id;
	}
}

nlohmann::json ModelsForPersistentCache(const std::vector<AcpModelState>& models)
{
	nlohmann::json result = nlohmann::json::array();
	for (const AcpModelState& model : models)
	{
		if (model.id.empty())
		{
			continue;
		}
		result.push_back({
		    {"id", model.id},
		    {"name", model.name},
		    {"description", model.description},
		    {"defaultReasoningEffort", model.default_reasoning_effort},
		    {"supportedReasoningEfforts", model.supported_reasoning_efforts},
		    {"additionalSpeedTiers", model.additional_speed_tiers},
		});
	}
	return result;
}

void RememberDiscoveredModels(AppState& app, const AcpSessionState& session)
{
	if (app.provider_model_catalog != nullptr && !session.available_models.empty())
	{
		(void)app.provider_model_catalog->RememberSuccessfulModels(session.provider_id, ModelsForPersistentCache(session.available_models));
	}
}

void FinishModelDiscoveryWithoutResults(AppState& app, const AcpSessionState& session)
{
	if (app.provider_model_catalog != nullptr && session.available_models.empty() && app.provider_model_catalog->IsDiscoveryPending(session.provider_id))
	{
		app.provider_model_catalog->RememberRefreshFailure(session.provider_id, "Provider model discovery completed without reporting any models.");
	}
}

void StopBackgroundModelDiscovery(AppState& app, AcpSessionState& session)
{
	if (!session.model_discovery_only)
	{
		return;
	}
	const std::string chat_id = session.chat_id;
	(void)StopAcpSession(app, chat_id);
	session.session_id.clear();
	session.codex_thread_id.clear();
	session.last_error.clear();
}

void HandleAcpResponse(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
{
	const nlohmann::json response_id = JsonRpcIdOrNull(message);
	const std::string request_id = JsonRpcIdToStableString(response_id);
	const int id = JsonRpcNumericId(response_id);
	if (id == 0)
	{
		AppendAcpDiagnostic(session, "response", "ignored_invalid_id", "", request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		return;
	}

	std::string method;
	if (const auto it = session.pending_request_methods.find(id); it != session.pending_request_methods.end())
	{
		method = it->second;
		session.pending_request_methods.erase(it);
	}
	if (session.prompt_request_id != 0 && id == session.prompt_request_id)
	{
		method = std::strcmp(ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpProtocolKind(), "codex-app-server") == 0 ? uam::acp_methods::kTurnStart : uam::acp_methods::kSessionPrompt;
	}

	if (const nlohmann::json* error_ptr = uam::nlohmann_json::FindField(message, "error"))
	{
		const nlohmann::json& error = *error_ptr;
		const nlohmann::json* code_json = uam::nlohmann_json::FindField(error, "code");
		const std::optional<int> parsed_code = code_json == nullptr ? std::nullopt : uam::nlohmann_json::IntValueStrict(*code_json);
		const bool has_code = parsed_code.has_value();
		const int code = parsed_code.value_or(0);
		const std::string default_error = std::string(RuntimeDisplayName(session)) + " request failed.";
		const std::string error_message = error.is_object() ? JsonDiagnosticStringValueOr(error, "message", default_error) : default_error;
		const std::string error_data = ErrorDataForDiagnostics(error);
		std::ostringstream detail;
		bool has_detail = false;
		if (!error_data.empty())
		{
			detail << "error.data=" << error_data;
			has_detail = true;
		}
		if (!session.recent_stderr.empty())
		{
			if (has_detail)
			{
				detail << "\n";
			}
			detail << "stderr_tail=" << RecentStderrTail(session);
			has_detail = true;
		}
		const std::string pending_summary = PendingRequestSummary(session);
		if (!pending_summary.empty())
		{
			if (has_detail)
			{
				detail << "\n";
			}
			detail << "pending_requests=" << pending_summary;
			has_detail = true;
		}
		const std::string detail_text = detail.str();
		AcpFailureDetails failure;
		failure.method = method;
		failure.request_id = request_id;
		failure.has_code = has_code;
		failure.code = code;
		failure.message = error_message;
		failure.has_detail = !detail_text.empty();
		const std::string formatted_error = FormatAcpFailureMessage(session, failure);
		const bool model_discovery_request = method == uam::acp_methods::kModelList || method == uam::acp_methods::kSessionNew || method == uam::acp_methods::kSessionLoad;
		if (app.provider_model_catalog != nullptr && (model_discovery_request || (session.model_discovery_only && method == uam::acp_methods::kInitialize)))
		{
			app.provider_model_catalog->RememberRefreshFailure(session.provider_id, formatted_error);
		}
		AppendAcpDiagnostic(session, "response", "jsonrpc_error", method, request_id, has_code, code, error_message, detail_text);
		if (std::strcmp(ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpProtocolKind(), "codex-app-server") == 0 && method == uam::acp_methods::kThreadResume && has_code && code == -32600 && uam::codex::ErrorLooksLikeInvalidThreadId(error_message) && !session.codex_resume_fallback_attempted)
		{
			session.codex_resume_fallback_attempted = true;
			session.session_setup_request_id = 0;
			session.session_id.clear();
			session.codex_thread_id.clear();
			chat.native_session_id.clear();
			SaveChatQuietly(app, chat);
			AppendAcpDiagnostic(session, "response", "codex_invalid_resume_id_retry_start", method, request_id, has_code, code, "Codex rejected the stored thread id. Starting a new thread instead.", detail_text);

			const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
			const std::string cwd = AcpWorkingDirectoryString(workspace_root);
			const int retry_id = NextAcpRequestId(session, uam::acp_methods::kThreadStart);
			session.session_setup_request_id = retry_id;
			session.lifecycle_state = kAcpLifecycleStarting;
			if (!WriteAcpMessage(session, BuildCodexThreadStartRequest(retry_id, chat, cwd)))
			{
				session.pending_request_methods.erase(retry_id);
				session.session_setup_request_id = 0;
				(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
				FailAcpTurnOrSession(session, uam::strings::NonEmptyOrFallback(session.last_error, formatted_error));
				SaveChatQuietly(app, chat);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			return;
		}
		AcpInvalidLoadRetryDetails invalid_load_retry;
		invalid_load_retry.failure = failure;
		invalid_load_retry.error_data = error_data;
		invalid_load_retry.detail_text = detail_text;
		invalid_load_retry.formatted_error = formatted_error;
		if (RetryGeminiSessionNewAfterInvalidLoad(app, session, chat, invalid_load_retry))
		{
			return;
		}
		if (session.model_discovery_only && (model_discovery_request || method == uam::acp_methods::kInitialize))
		{
			StopBackgroundModelDiscovery(app, session);
			return;
		}
		if (method == uam::acp_methods::kSessionSetModel && id == session.startup_model_request_id)
		{
			ClearAcpStartupModelRequest(session);
		}
		if (method == uam::acp_methods::kSessionPrompt || session.processing || session.waiting_for_permission || session.waiting_for_user_input || !session.queued_prompt.empty())
		{
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			FailAcpTurnOrSession(session, formatted_error);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
		}
		else
		{
			session.last_error = formatted_error;
			session.lifecycle_state = kAcpLifecycleError;
		}
		return;
	}

	const nlohmann::json result = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(message, "result"));
	if (uam::acp_methods::IsLifecycleResultMethod(method))
	{
		AppendAcpDiagnostic(session, "response", "jsonrpc_result", method, request_id, false, 0, "", "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes));
	}
	if (method == uam::acp_methods::kInitialize)
	{
		session.initialize_request_id = 0;
		session.initialized = true;
		session.lifecycle_state = kAcpLifecycleStarting;
		if (std::strcmp(ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpProtocolKind(), "codex-app-server") == 0)
		{
			session.agent_name = "codex";
			session.agent_title = "Codex";
			if (result.is_object())
			{
				session.agent_version = JsonDiagnosticStringValue(result, "userAgent");
			}
			session.load_session_supported = true;
			(void)WriteAcpMessage(session, BuildCodexInitializedNotification());
			const int model_list_id = NextAcpRequestId(session, uam::acp_methods::kModelList);
			(void)WriteAcpMessage(session, BuildCodexModelListRequest(model_list_id));
			return;
		}
		if (result.is_object())
		{
			const nlohmann::json agent_info = JsonObjectValue(result, "agentInfo");
			if (agent_info.is_object())
			{
				session.agent_name = JsonDiagnosticStringValue(agent_info, "name");
				session.agent_title = JsonDiagnosticStringValue(agent_info, "title");
				session.agent_version = JsonDiagnosticStringValue(agent_info, "version");
			}
			const nlohmann::json agent_capabilities = JsonObjectValue(result, "agentCapabilities");
			if (agent_capabilities.is_object())
			{
				session.load_session_supported = JsonBooleanValueOr(agent_capabilities, "loadSession", false);
			}
		}
		return;
	}

	if (method == uam::acp_methods::kModelList)
	{
		if (result.is_object())
		{
			const nlohmann::json data = JsonArrayValue(result, "data");
			if (data.is_array())
			{
				session.available_models.clear();
				std::vector<std::string> seen_model_ids;
				uam::acp_models::CodexModelParseOptions parse_options;
				parse_options.skip_hidden_field = true;
				parse_options.allow_default_non_list_visibility = true;
				for (const nlohmann::json& model : data)
				{
					const auto parsed = uam::acp_models::ParseCodexModelEntry(model, parse_options);
					if (!parsed)
					{
						continue;
					}
					if (uam::ranges::Contains(seen_model_ids, parsed->model.id))
					{
						continue;
					}

					if (parsed->is_default)
					{
						session.current_model_id = parsed->model.id;
					}
					seen_model_ids.push_back(parsed->model.id);
					session.available_models.push_back(std::move(parsed->model));
				}

				const std::string explicit_current_model = uam::nlohmann_json::TrimmedStringValue(result, {"currentModelId", "model"});
				if (!explicit_current_model.empty())
				{
					session.current_model_id = explicit_current_model;
				}
			}
		}
		RememberDiscoveredModels(app, session);
		FinishModelDiscoveryWithoutResults(app, session);
		StopBackgroundModelDiscovery(app, session);
		return;
	}

	if (uam::acp_methods::IsCodexThreadSetupMethod(method))
	{
		session.session_setup_request_id = 0;
		std::string returned_thread_id;
		if (result.is_object())
		{
			const nlohmann::json thread = JsonObjectValue(result, "thread");
			if (thread.is_object())
			{
				returned_thread_id = JsonDiagnosticStringValue(thread, "id");
			}
			session.current_model_id = uam::nlohmann_json::TrimmedStringValueOr(result, "model", session.current_model_id);
		}
		if (uam::codex::IsValidThreadId(returned_thread_id))
		{
			session.codex_thread_id = returned_thread_id;
			session.session_id = session.codex_thread_id;
		}
		else
		{
			session.codex_thread_id.clear();
			session.session_id.clear();
		}
		const std::string previous_native_session_id = chat.native_session_id;
		SetChatNativeSessionIdIfChanged(chat, session.session_id);
		SyncResolvedNativeSessionIdForChat(app, chat, session.session_id, previous_native_session_id);
		session.available_modes = {
		    AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Codex default collaboration mode."},
		    AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Ask Codex to plan before implementing."},
		};
		session.current_mode_id = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
		session.session_ready = !session.session_id.empty();
		session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
		if (!session.session_ready)
		{
			const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
			AcpFailureDetails failure;
			failure.method = method;
			failure.request_id = request_id;
			failure.message = "Codex app-server did not return a valid thread id.";
			failure.has_detail = true;
			session.last_error = FormatAcpFailureMessage(session, failure);
			AppendAcpDiagnostic(session, "response", "missing_thread_id", method, request_id, false, 0, session.last_error, detail);
		}
		SaveChatQuietly(app, chat);
		return;
	}

	if (method == uam::acp_methods::kTurnStart)
	{
		session.prompt_request_id = 0;
		if (result.is_object())
		{
			const nlohmann::json turn = JsonObjectValue(result, "turn");
			if (turn.is_object())
			{
				session.codex_turn_id = JsonDiagnosticStringValueOr(turn, "id", session.codex_turn_id);
			}
		}
		session.lifecycle_state = kAcpLifecycleProcessing;
		return;
	}

	if (method == uam::acp_methods::kSessionNew)
	{
		session.session_setup_request_id = 0;
		if (result.is_object())
		{
			session.session_id = uam::nlohmann_json::TrimmedStringValueOr(result, "sessionId", session.session_id);
			UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
			UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
		}
		RememberDiscoveredModels(app, session);
		FinishModelDiscoveryWithoutResults(app, session);
		if (!session.model_discovery_only)
		{
			const std::string previous_native_session_id = chat.native_session_id;
			SetChatNativeSessionIdIfChanged(chat, session.session_id);
			SyncResolvedNativeSessionIdForChat(app, chat, session.session_id, previous_native_session_id);
		}
		session.session_ready = !session.session_id.empty();
		session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
		if (!session.session_ready)
		{
			const std::string detail = "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes) + (session.recent_stderr.empty() ? "" : "\nstderr_tail=" + RecentStderrTail(session));
			AcpFailureDetails failure;
			failure.method = method;
			failure.request_id = request_id;
			failure.message = std::string(RuntimeDisplayName(session)) + " did not return a session id.";
			failure.has_detail = true;
			session.last_error = FormatAcpFailureMessage(session, failure);
			AppendAcpDiagnostic(session, "response", "missing_session_id", method, request_id, false, 0, session.last_error, detail);
		}
		if (!session.model_discovery_only)
		{
			SaveChatQuietly(app, chat);
		}
		StopBackgroundModelDiscovery(app, session);
		return;
	}

	if (method == uam::acp_methods::kSessionLoad)
	{
		session.session_setup_request_id = 0;
		if (result.is_object())
		{
			UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
			UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
		}
		RememberDiscoveredModels(app, session);
		FinishModelDiscoveryWithoutResults(app, session);
		session.session_ready = true;
		session.ignore_session_updates_until_ready = false;
		session.lifecycle_state = kAcpLifecycleReady;
		StopBackgroundModelDiscovery(app, session);
		return;
	}

	if (method == uam::acp_methods::kSessionPrompt)
	{
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, nullptr);
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
		return;
	}

	if (method == uam::acp_methods::kSessionCancel)
	{
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0.0;
		session.cancel_request_id = 0;
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return;
	}

	if (method == uam::acp_methods::kTurnInterrupt)
	{
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0.0;
		session.cancel_request_id = 0;
		session.codex_turn_id.clear();
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return;
	}

	if (uam::acp_methods::IsSessionModeOrModelUpdateMethod(method))
	{
		if (method == uam::acp_methods::kSessionSetModel && JsonRpcNumericId(JsonRpcIdOrNull(message)) == session.startup_model_request_id)
		{
			ClearAcpStartupModelRequest(session);
		}
		return;
	}

	AppendAcpDiagnostic(session, "response", "unknown_request_id", method, request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
}

} // namespace uam::acp_detail
