#include "app/agent_run_scheduler.h"
#include "app/provider_configuration_service.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_model_json.h"

#include <cstring>
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"

#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace uam
{
	/// <summary>Owns the full native binding save without borrowing GUI chat/session references.</summary>
	struct PendingLocalSessionReceipt
	{
		std::filesystem::path data_root;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::string ownership;
		std::string queue_revision;
		std::string queue_fingerprint;
		std::string prompt;
		std::uint64_t turn_serial = 0;
		std::size_t queue_count = 0;
		bool callback_complete = false;
		acp_detail::DurableAdmissionState admission = acp_detail::DurableAdmissionState::Pending;
	};
}

namespace uam::acp_detail
{
namespace
{
	bool CanDeferLocalSessionReceipt(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		return (app.settings.central_provider_configuration.enabled || ShouldDeferLocalAcpConfiguration(app, session, chat)) &&
		    chat.execution_host_id == execution_hosts::kLocalHostId &&
		    session.process_execution_host_id == execution_hosts::kLocalHostId && !chat.small_model_mode &&
		    !session.model_discovery_only && !session.goal_internal_session &&
		    session.goal_turn_kind.empty() && !session.goal_review_turn &&
		    chat.active_goal_id.empty() && chat.goal_owner_chat_id.empty() &&
		    chat.goal_iteration_goal_id.empty() && !session.recovering_remote_turn &&
		    !session.recovering_remote_process && session.running && session.session_ready;
	}

	bool OwnsLocalSessionScope(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, const PendingLocalSessionReceipt& receipt)
	{
		return CanDeferLocalSessionReceipt(app, session, chat) && !session.cancel_requested &&
		    !session.local_stop_pending && receipt.data_root == app.data_root &&
		    receipt.incarnation == chat.metadata_incarnation &&
		    receipt.ownership == LocalSessionReceiptOwnership(app, session, chat);
	}

	bool OwnsLocalSessionReceipt(const AppState& app, const AcpSessionState& session,
	    const ChatSession& chat, const PendingLocalSessionReceipt& receipt)
	{
		return OwnsLocalSessionScope(app, session, chat, receipt) && receipt.turn_serial == session.turn_serial &&
		    receipt.queue_revision == chat.acp_queue_storage_revision &&
		    receipt.queue_fingerprint == ChatRepository::QueueAdmissionFingerprint(chat) &&
		    receipt.queue_count == session.queued_user_prompts.size() && receipt.prompt == session.queued_prompt;
	}

	void StageLocalSessionReceipt(AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		const std::shared_ptr<PendingLocalSessionReceipt> receipt = std::make_shared<PendingLocalSessionReceipt>();
		receipt->data_root = app.data_root;
		receipt->incarnation = chat.metadata_incarnation;
		receipt->ownership = LocalSessionReceiptOwnership(app, session, chat);
		receipt->queue_revision = chat.acp_queue_storage_revision;
		receipt->queue_fingerprint = ChatRepository::QueueAdmissionFingerprint(chat);
		receipt->queue_count = session.queued_user_prompts.size();
		receipt->prompt = session.queued_prompt;
		receipt->turn_serial = session.turn_serial;
		session.pending_local_session_receipt = receipt;
		const std::weak_ptr<PendingLocalSessionReceipt> weak = receipt;
		const std::string chat_id = chat.id;
		receipt->admission = BeginDurableAdmission(app, chat, [&app, chat_id, weak]()
		{
			const std::shared_ptr<PendingLocalSessionReceipt> saved = weak.lock();
			const ChatSession* current = ChatDomainService().FindChatById(app, chat_id);
			const AcpSessionState* owner = FindAcpSessionForChat(app, chat_id);
			return saved && current && owner && owner->pending_local_session_receipt == saved &&
			    OwnsLocalSessionReceipt(app, *owner, *current, *saved);
		}, [weak](DurableAdmissionState result)
		{
			if (const std::shared_ptr<PendingLocalSessionReceipt> saved = weak.lock())
			{
				saved->admission = result;
				saved->callback_complete = true;
			}
		});
		if (receipt->admission == DurableAdmissionState::Failed) receipt->callback_complete = true;
	}
}

bool PollLocalSessionReceipt(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	const std::shared_ptr<PendingLocalSessionReceipt> receipt = session.pending_local_session_receipt;
	if (!receipt) return false;
	if (OwnsLocalSessionScope(app, session, chat, *receipt) && session.processing &&
	    receipt->turn_serial == session.turn_serial && receipt->prompt == session.queued_prompt &&
	    (receipt->queue_revision != chat.acp_queue_storage_revision ||
	     receipt->queue_count != session.queued_user_prompts.size() ||
	     receipt->queue_fingerprint != ChatRepository::QueueAdmissionFingerprint(chat)))
	{
		// Let the old callback retire its admission guard before adding the replacement.
		if (!receipt->callback_complete) return true;
		// A legitimate queued edit revokes the older snapshot and requires a new full receipt.
		StageLocalSessionReceipt(app, session, chat);
		return true;
	}
	if (receipt->admission == DurableAdmissionState::Failed && receipt->callback_complete && session.processing &&
	    !session.queued_prompt.empty() && session.turn_serial != receipt->turn_serial &&
	    receipt->data_root == app.data_root && receipt->incarnation == chat.metadata_incarnation &&
	    CanDeferLocalSessionReceipt(app, session, chat))
	{
		// A fresh explicit turn obtains its own complete receipt; never revive failed input.
		StageLocalSessionReceipt(app, session, chat);
		return true;
	}
	if (!OwnsLocalSessionReceipt(app, session, chat, *receipt))
	{
		if (receipt->admission != DurableAdmissionState::Failed)
		{
			receipt->admission = DurableAdmissionState::Failed;
			session.last_error = "Session setup ownership changed before its durable receipt committed.";
		}
		return true;
	}
	if (receipt->admission != DurableAdmissionState::Committed) return true;
	session.pending_local_session_receipt.reset();
	(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
	ScheduleCompletedAcpPromptRecoverySaveIfReady(app, chat.id);
	return false;
}


void HandleAcpRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
{
	if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, message)) return;
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

bool ReplayPersistedInteractionResponseIfMatched(
    AppState& app, AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& request, const AcpPendingPermissionState* pending)
{
	const std::string request_id =
	    JsonRpcIdToStableString(JsonRpcIdOrNull(request));
	const bool captured_read = session.goal_review_turn || session.active_uam_agent_workspace_access == "read";
	// Early request-ID matching is insufficient authority for a captured read turn.
	// Questions and unknown/malformed requests continue through their normal decoder.
	if (captured_read && (pending == nullptr || pending->request_id_json.empty() || pending->request_id_json != request_id)) return false;
	const auto saved = std::ranges::find(
	    chat.remote_interaction_responses, request_id,
	    &uam::AcpRemoteInteractionResponseState::request_id_json);
	if (saved == chat.remote_interaction_responses.end())
		return false;
	try
	{
		const nlohmann::json response = nlohmann::json::parse(saved->response_json);
		if (captured_read)
		{
			if (!CapturedReadCeilingAllowsSavedPermissionResponse(session, *pending, response))
			{
				AppendAcpDiagnostic(session, "permission", "saved_response_exceeds_captured_read_ceiling", "",
				    request_id, false, 0, "Saved response was not replayed under the current captured read-only restriction.");
				return false;
			}
		}
		std::string error;
		if (!WriteAcpMessage(session, response, &error))
		{
			RecoverDisconnectedRemoteAcpTransport(
			    app, session, chat,
			    "Failed to replay the saved remote interaction response: " +
			        uam::strings::NonEmptyOrFallback(error, "unknown transport error"));
		}
		else
		{
			AppendAcpDiagnostic(session, "write", "interaction_response_replayed", "",
			                    saved->request_id_json, false, 0,
			                    "Replayed the saved response without asking the user again.");
		}
	}
	catch (const nlohmann::json::exception&)
	{
		chat.remote_interaction_responses.erase(saved);
		(void)SaveChatQuietly(app, chat);
		if (captured_read) return false;
	}
	return true;
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
			parsed.provider_id = provider_mode_id;
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

bool UpdateAcpConfigOptions(AcpSessionState& session, const nlohmann::json& config_options)
{
	if (!config_options.is_array()) return false;
	std::vector<AcpConfigOptionState> parsed;
	parsed.reserve(std::min<std::size_t>(config_options.size(), 64));
	bool model_option_seen = false;
	for (const nlohmann::json& value : config_options)
	{
		if (parsed.size() >= 64) break;
		if (!value.is_object()) continue;
		AcpConfigOptionState option;
		option.id = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(value, {"id"}), 256);
		if (option.id.empty()) continue;
		option.name = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValueOr(value, "name", option.id), 256);
		option.description = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(value, {"description"}), 1024);
		option.category = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(value, {"category"}), 256);
		option.current_value = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(value, {"currentValue"}), 512);
		for (const nlohmann::json& raw_choice : JsonArrayValue(value, "options"))
		{
			if (option.choices.size() >= 128) break;
			if (!raw_choice.is_object()) continue;
			AcpConfigOptionChoiceState choice;
			choice.value = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(raw_choice, {"value"}), 512);
			if (choice.value.empty()) continue;
			choice.name = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValueOr(raw_choice, "name", choice.value), 256);
			choice.description = CapDiagnosticString(uam::nlohmann_json::TrimmedStringValue(raw_choice, {"description"}), 1024);
			option.choices.push_back(std::move(choice));
		}
		if (!model_option_seen && (option.category == "model" || option.id == "model"))
		{
			model_option_seen = true;
			if (!option.current_value.empty()) session.current_model_id = option.current_value;
		}
		if (!option.current_value.empty())
		{
			if (option.category == "mode" || option.id == "mode") session.current_mode_id = AppApprovalModeId(option.current_value);
		}
		parsed.push_back(std::move(option));
	}
	session.available_config_options = std::move(parsed);
	return true;
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
		    {"contextWindowTokens", model.context_window_tokens ? nlohmann::json(*model.context_window_tokens) : nlohmann::json(nullptr)},
		    {"maxOutputTokens", model.max_output_tokens ? nlohmann::json(*model.max_output_tokens) : nlohmann::json(nullptr)},
		});
	}
	return result;
}

nlohmann::json ConfigOptionsForPersistentCache(const std::vector<AcpConfigOptionState>& options)
{
	nlohmann::json result = nlohmann::json::array();
	for (const AcpConfigOptionState& option : options)
	{
		nlohmann::json choices = nlohmann::json::array();
		for (const AcpConfigOptionChoiceState& choice : option.choices)
		{
			choices.push_back({{"value", choice.value}, {"name", choice.name}, {"description", choice.description}});
		}
		result.push_back({{"id", option.id}, {"name", option.name}, {"description", option.description}, {"category", option.category}, {"currentValue", option.current_value}, {"options", std::move(choices)}});
	}
	return result;
}

std::string DiscoveryWorkspace(const AppState& app, const ChatSession& chat)
{
	return uam::paths::ResolveWorkspaceRootPath(app, chat).generic_string();
}

void RememberDiscoveredModels(AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	const auto& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	if (runtime.ModelOptionsRequireRestart())
	{
		const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
		const std::string version = host != nullptr ? CheckedProviderConfigurationVersion(app, session.provider_id, *host) : std::string{};
		for (auto& model : session.available_models)
		{
			model.supported_reasoning_efforts = runtime.ModelReasoningEfforts(model.id, version);
			model.default_reasoning_effort.clear();
			model.additional_speed_tiers.clear();
		}
	}
	if (app.provider_model_catalog != nullptr && (!session.available_models.empty() || !session.available_config_options.empty()))
	{
		(void)app.provider_model_catalog->RememberSuccessfulModels(session.provider_id,
		    ModelsForPersistentCache(session.available_models), DiscoveryWorkspace(app, chat),
		    ConfigOptionsForPersistentCache(session.available_config_options),
		    chat.execution_host_id, session.model_catalog_scope_key);
	}
}

void FinishModelDiscoveryWithoutResults(AppState& app, const AcpSessionState& session, const ChatSession& chat)
{
	const std::string workspace = DiscoveryWorkspace(app, chat);
	if (app.provider_model_catalog != nullptr && session.available_models.empty() &&
	    app.provider_model_catalog->IsDiscoveryPending(
	        session.provider_id, workspace, chat.execution_host_id))
	{
		app.provider_model_catalog->RememberRefreshFailure(session.provider_id,
		    "Provider model discovery completed without reporting any models.", workspace,
		    chat.execution_host_id, session.model_catalog_scope_key);
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

void FinishAcpSessionLoad(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	session.load_replay_quiet_until_s = 0.0;
	session.session_ready = true;
	session.ignore_session_updates_until_ready = false;
	session.lifecycle_state = kAcpLifecycleReady;
	(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpReconcileModelOptions(app, session, chat);
	StopBackgroundModelDiscovery(app, session);
	(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
}

bool SettleAcpLoadReplay(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds)
{
	if (session.load_replay_quiet_until_s <= 0.0 || now_seconds < session.load_replay_quiet_until_s) return false;
	FinishAcpSessionLoad(app, session, chat);
	return true;
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

	if (std::any_of(chat.remote_pending_requests.begin(), chat.remote_pending_requests.end(),
	        [id](const AcpRemotePendingRequestState& request)
	        { return request.request_id == id && request.response_consumed; })) return;
	std::string method;
	if (const auto it = session.pending_request_methods.find(id); it != session.pending_request_methods.end())
	{
		method = it->second;
		session.pending_request_methods.erase(it);
	}
	if (method.empty() && session.prompt_request_id != 0 && id == session.prompt_request_id)
	{
		method = std::strcmp(ProviderRuntimeRegistry::ResolveById(session.provider_id).AcpProtocolKind(), "codex-app-server") == 0 ? uam::acp_methods::kTurnStart : uam::acp_methods::kSessionPrompt;
	}
	if (method.empty() && session.recovering_remote_turn && session.processing)
	{
		// Legacy saves lack request correlation. Only provider-recognized completion
		// may settle the turn; an arbitrary error could belong to steering or discovery.
		method = ProviderRuntimeRegistry::ResolveById(session.provider_id).RecoverAcpResponseMethod(message);
	}
	if (method.empty())
	{
		AppendAcpDiagnostic(session, "response", "ignored_unknown_id", "", request_id, false, 0);
		return;
	}
	if (session.cancel_requested && method == uam::acp_methods::kSessionPrompt)
	{
		if (const nlohmann::json* result = uam::nlohmann_json::FindObjectField(message, "result"))
			ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpObserveCancelledPromptResult(app, session, chat, *result);
		if (const nlohmann::json* error = uam::nlohmann_json::FindField(message, "error"); error != nullptr &&
		    ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHoldUnconfirmedInput(app, session, chat,
		        JsonDiagnosticStringValueOr(*error, "message", "Cancellation returned an unconfirmed prompt error.")))
			return;
		if (session.inactivity_timeout_pending)
		{
			FinalizeAcpTurnInactivityTimeout(app, session, chat);
			return;
		}
		session.prompt_request_id = 0;
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0.0;
		session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return;
	}

	if (const nlohmann::json* error_ptr = uam::nlohmann_json::FindField(message, "error"))
	{
		if ((method == uam::acp_methods::kTurnStart || method == uam::acp_methods::kSessionPrompt) &&
		    !chat.provider_handoff_context.empty() && chat.provider_handoff_session_id == session.session_id)
		{
			// A rejected prompt may not have reached the provider's conversation history.
			chat.provider_handoff_session_id.clear();
			ScheduleChatSave(app, chat, 0.0);
		}
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
		std::string formatted_error = FormatAcpFailureMessage(session, failure);
		if (method == uam::acp_methods::kSessionSetModel && has_code && code == -32602 &&
		    uam::strings::TrimAndLowerAscii(error_message) == "unsupported model")
		{
			formatted_error += " Refresh models in Settings or restart UAM, then select an available model before retrying.";
		}
		const bool model_discovery_request = method == uam::acp_methods::kSessionNew || method == uam::acp_methods::kSessionLoad || method == uam::acp_methods::kSessionResume;
		if (app.provider_model_catalog != nullptr && (model_discovery_request || (session.model_discovery_only && method == uam::acp_methods::kInitialize)))
		{
			app.provider_model_catalog->RememberRefreshFailure(session.provider_id,
			    formatted_error, DiscoveryWorkspace(app, chat), chat.execution_host_id, session.model_catalog_scope_key);
		}
		AppendAcpDiagnostic(session, "response", "jsonrpc_error", method, request_id, has_code, code, error_message, detail_text);
		const AcpResponseFailureDetails details{failure, error_data, detail_text, formatted_error};
		if (ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleError(
		        app, session, chat, details)) return;
		if (session.model_discovery_only && (model_discovery_request || method == uam::acp_methods::kInitialize))
		{
			StopBackgroundModelDiscovery(app, session);
			return;
		}
		if (method == uam::acp_methods::kInitialize ||
		    (id != 0 && id == session.session_setup_request_id))
		{
			InvalidateAcpTransport(app, session, chat, formatted_error);
			return;
		}
		if (method == uam::acp_methods::kSessionSetConfigOption && id == session.reasoning_change_request_id)
		{
			for (AcpModelState& model : session.available_models)
			{
				if (model.id == session.current_model_id)
				{
					model.default_reasoning_effort = session.reasoning_change_previous_id;
					break;
				}
			}
			if (session.reasoning_change_previous_chat_id.has_value() && chat.reasoning_effort == session.reasoning_change_requested_id)
			{
				chat.reasoning_effort = *session.reasoning_change_previous_chat_id;
				SaveChatQuietly(app, chat);
			}
			ClearAcpReasoningChangeRequest(session);
		}
		if (method == uam::acp_methods::kSessionSetConfigOption && id == session.config_option_change_request_id)
		{
			ClearAcpConfigOptionChangeRequest(session);
		}
		if (method == uam::acp_methods::kSessionSetMode && id == session.mode_change_request_id)
		{
			if (RollbackAcpModeChange(session, chat))
			{
				SaveChatQuietly(app, chat);
			}
		}
		if (method == uam::acp_methods::kSessionSetModel && id == session.model_change_request_id)
		{
			const std::string previous_model_id = session.model_change_previous_id;
			const std::string rejected_model_id = session.model_change_requested_id;
			session.current_model_id = previous_model_id;
			if (session.model_change_previous_chat_id.has_value() && chat.model_id == rejected_model_id)
			{
				chat.model_id = *session.model_change_previous_chat_id;
				SaveChatQuietly(app, chat);
			}
			ClearAcpModelChangeRequest(session);
			session.awaiting_model_config_options = false;
		}
		if (method == uam::acp_methods::kSessionSetModel && id == session.startup_model_request_id)
		{
			ClearAcpStartupModelRequest(session);
		}
		if (method == uam::acp_methods::kSessionPrompt || session.processing || session.waiting_for_permission || session.waiting_for_user_input || !session.queued_prompt.empty())
		{
			(void)FinalizeActiveAcpToolCallsAsFailed(chat, session);
			FailAcpTurnOrSession(session, &chat, formatted_error);
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

	struct InitialNativeResultObservation
	{
		AcpSessionState& session;
		const std::string& method;
		int id;
		~InitialNativeResultObservation()
		{
			AgentRunScheduler::InitialNativeResponse(session, method, id);
		}
	} initial_observation{session, method, id};
	const nlohmann::json result = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(message, "result"));
	if (uam::acp_methods::IsLifecycleResultMethod(method))
	{
		AppendAcpDiagnostic(session, "response", "jsonrpc_result", method, request_id, false, 0, "", "result=" + CapDiagnosticString(result.dump(), kMaxAcpDiagnosticDetailBytes));
	}
	else if (uam::acp_methods::IsSessionModeOrModelUpdateMethod(method))
	{
		// Acknowledgement only: a startup timeline must show which setup write was answered before the prompt.
		AppendAcpDiagnostic(session, "response", "jsonrpc_result", method, request_id);
	}
	if (method == uam::acp_methods::kInitialize)
	{
		session.initialize_request_id = 0;
		session.initialized = true;
		session.lifecycle_state = kAcpLifecycleStarting;
		ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpInitializeResult(session, result);
		if (!session.model_discovery_only && chat.remote_process_exists && !chat.remote_process_control_token.empty() && session.remote_output_delivery_token == chat.remote_process_control_token && !session.process_execution_host_id.empty() && session.process_execution_host_id != execution_hosts::kLocalHostId && session.process_execution_host_id == chat.execution_host_id && session.provider_id == chat.provider_id)
		{
			chat.remote_prompt_image_supported = session.prompt_image_supported;
			if (!SaveChatQuietly(app, chat))
				ScheduleChatSave(app, chat, 0.0);
		}
		if (session.lifecycle_state == kAcpLifecycleError)
		{
			RecoverDisconnectedRemoteAcpTransport(app, session, chat, session.last_error);
		}
		return;
	}
	if (ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpHandleResult(app, session, chat, method, request_id, result)) return;

	if (method == uam::acp_methods::kSessionNew)
	{
		session.session_setup_request_id = 0;
		if (result.is_object())
		{
			session.session_id = uam::nlohmann_json::TrimmedStringValueOr(result, "sessionId", session.session_id);
			UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
			UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
			if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(result, "configOptions"))
			{
				(void)UpdateAcpConfigOptions(session, *config_options);
				(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpConfigOptionsUpdated(session, *config_options);
			}
		}
		RememberDiscoveredModels(app, session, chat);
		FinishModelDiscoveryWithoutResults(app, session, chat);
		if (!session.model_discovery_only && !session.goal_internal_session)
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
			(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpReconcileModelOptions(app, session, chat);
			if (CanDeferLocalSessionReceipt(app, session, chat)) StageLocalSessionReceipt(app, session, chat);
			else SaveChatQuietly(app, chat);
		}
		StopBackgroundModelDiscovery(app, session);
		(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
		return;
	}

	if (method == uam::acp_methods::kSessionLoad || method == uam::acp_methods::kSessionResume)
	{
		session.session_setup_request_id = 0;
		if (result.is_object())
		{
			UpdateAcpModesFromJson(session, JsonObjectValue(result, "modes"));
			UpdateAcpModelsFromJson(session, JsonObjectValue(result, "models"));
			if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(result, "configOptions"))
			{
				(void)UpdateAcpConfigOptions(session, *config_options);
				(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpConfigOptionsUpdated(session, *config_options);
			}
		}
		RememberDiscoveredModels(app, session, chat);
		FinishModelDiscoveryWithoutResults(app, session, chat);
		// Gemini CLI replays its own history (including turns UAM saved differently, such as a
		// timed-out answer) only after this response. Keep ignoring it until the replay is quiet,
		// so none of it lands in the next turn.
		if (method == uam::acp_methods::kSessionLoad && session.ignore_session_updates_until_ready &&
		    provider_ids::NormalizeCliProviderAlias(session.provider_id) == provider_ids::kGeminiCli)
		{
			session.load_replay_quiet_until_s = GetAppTimeSeconds() + kLoadReplayQuietSeconds;
			return;
		}
		FinishAcpSessionLoad(app, session, chat);
		return;
	}

	if (method == uam::acp_methods::kSessionPrompt)
	{
		const bool defer_completion = CanDeferLocalSessionReceipt(app, session, chat);
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		if (id == session.prompt_request_id && session.prompt_request_id > 0 && JsonDiagnosticStringValue(result, "stopReason") == "end_turn")
			RecordCompletedAcpPromptRecoveries(app, session, chat);
		CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, nullptr);
		if (defer_completion) ScheduleChatSave(app, chat, 0.0);
		else SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
		return;
	}

	if (method == uam::acp_methods::kSessionCancel)
	{
		if (session.inactivity_timeout_pending)
		{
			FinalizeAcpTurnInactivityTimeout(app, session, chat);
			return;
		}
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0.0;
		session.cancel_request_id = 0;
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return;
	}

		if (uam::acp_methods::IsSessionModeOrModelUpdateMethod(method))
	{
		const int response_id = JsonRpcNumericId(JsonRpcIdOrNull(message));
		if (method == uam::acp_methods::kSessionSetConfigOption && response_id == session.reasoning_change_request_id)
		{
			if (result.is_object())
			{
				if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(result, "configOptions"))
				{
					(void)UpdateAcpConfigOptions(session, *config_options);
					(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpConfigOptionsUpdated(session, *config_options);
				}
			}
			ClearAcpReasoningChangeRequest(session);
			session.last_error.clear();
			session.lifecycle_state = kAcpLifecycleReady;
			(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpReconcileModelOptions(app, session, chat);
			(void)SendQueuedPromptIfReady(app, session, chat);
		}
		if (method == uam::acp_methods::kSessionSetConfigOption && response_id == session.config_option_change_request_id)
		{
			bool received_config_options = false;
			if (result.is_object())
			{
				if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(result, "configOptions"))
				{
					received_config_options = true;
					(void)UpdateAcpConfigOptions(session, *config_options);
				}
			}
			if (!received_config_options) return;
			const auto confirmed = std::ranges::find_if(session.available_config_options, [&](const AcpConfigOptionState& option) {
				return option.id == session.config_option_change_id && option.current_value == session.config_option_change_requested_value;
			});
			if (confirmed == session.available_config_options.end())
			{
				session.last_error = "The provider did not confirm the requested model variant.";
			}
			else
			{
				session.last_error.clear();
			}
			ClearAcpConfigOptionChangeRequest(session);
			session.lifecycle_state = kAcpLifecycleReady;
		}
		if (method == uam::acp_methods::kSessionSetMode && response_id == session.mode_change_request_id)
		{
			session.current_mode_id = session.mode_change_requested_id;
			ClearAcpModeChangeRequest(session);
			session.last_error.clear();
			session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
			(void)SendQueuedPromptIfReady(app, session, chat);
		}
		if (method == uam::acp_methods::kSessionSetModel)
		{
			if (response_id == session.model_change_request_id)
			{
				session.current_model_id = session.model_change_requested_id;
				bool received_config_options = false;
				if (result.is_object())
				{
					if (const nlohmann::json* config_options = uam::nlohmann_json::FindArrayField(result, "configOptions"))
					{
						received_config_options = true;
						(void)UpdateAcpConfigOptions(session, *config_options);
						(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpConfigOptionsUpdated(session, *config_options);
					}
				}
				if (received_config_options)
				{
					session.awaiting_model_config_options = false;
				}
				ClearAcpModelChangeRequest(session);
				session.last_error.clear();
				session.lifecycle_state = kAcpLifecycleReady;
			}
			if (response_id == session.startup_model_request_id)
			{
				ClearAcpStartupModelRequest(session);
			}
			(void)ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpReconcileModelOptions(app, session, chat);
			(void)SendQueuedPromptIfReady(app, session, chat);
		}
		return;
	}

	AppendAcpDiagnostic(session, "response", "unknown_request_id", method, request_id, false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
}

} // namespace uam::acp_detail
