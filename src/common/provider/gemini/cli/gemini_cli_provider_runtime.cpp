#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#include "app/agent_run_scheduler.h"
#include "common/runtime/acp/acp_session_internal.h"

#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/chat/chat_ids.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/gemini/base/gemini_history_loader.h"
#include "common/provider/provider_ids.h"
#include "common/provider/structured_attachments.h"
#include "common/platform/async_byte_writer.h"
#include "common/utils/base64.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/json_runtime.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/usage/transcript_usage.h"
#include "common/utils/time_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/io_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace
{
	std::string_view ManagedGeminiCompletedModel(const nlohmann::json& result)
	{
		if (uam::nlohmann_json::StringViewOrEmpty(result, "stopReason") != "end_turn" || result.contains("configOptions") || result.contains("models") || result.contains("modes")) return {};
		const auto* meta = uam::nlohmann_json::FindObjectField(result, "_meta");
		const auto* quota = meta ? uam::nlohmann_json::FindObjectField(*meta, "quota") : nullptr;
		const auto* models = quota ? uam::nlohmann_json::FindArrayField(*quota, "model_usage") : nullptr;
		const auto* total = quota ? uam::nlohmann_json::FindObjectField(*quota, "token_count") : nullptr;
		if (!models || models->size() != 1 || !total) return {};
		const auto* counts = uam::nlohmann_json::FindObjectField(models->front(), "token_count");
		if (!counts) return {};
		const auto count = [](const nlohmann::json& object, const char* key) -> std::optional<std::int64_t>
		{
			const auto* value = uam::nlohmann_json::FindField(object, key);
			if (!value || !value->is_number_integer() || (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(uam::usage::kMaxExactTokenCount))) return std::nullopt;
			const auto number = value->get<std::int64_t>();
			return number >= 0 && number <= uam::usage::kMaxExactTokenCount ? std::optional<std::int64_t>{number} : std::nullopt;
		};
		const auto input = count(*counts, "input_tokens"), output = count(*counts, "output_tokens");
		if (!input || !output || *output > uam::usage::kMaxExactTokenCount - *input || *input + *output == 0 || count(*total, "input_tokens") != input || count(*total, "output_tokens") != output) return {};
		if (result.contains("usage"))
		{
			const auto* usage = uam::nlohmann_json::FindObjectField(result, "usage");
			if (!usage || count(*usage, "inputTokens") != input || count(*usage, "outputTokens") != output || count(*usage, "totalTokens") != std::optional<std::int64_t>{*input + *output}) return {};
		}
		return uam::nlohmann_json::StringViewOrEmpty(models->front(), "model");
	}

	nlohmann::json GeminiSavedImageOwner(const uam::AcpSessionState& session, const ChatSession& settings)
	{
		const auto process = uam::acp_detail::AcpProcessHandleLabel(session);
		if (session.protocol_kind != "gemini-acp" || !uam::provider_ids::IsCliProviderAliasOf(session.provider_id, uam::provider_ids::kGeminiCli) ||
		    !session.running || !session.initialized || !session.session_ready || !session.prompt_image_supported || session.cancel_requested || session.local_stop_pending ||
		    settings.workspace_directory.empty() || process.empty() || process != session.last_process_id ||
		    session.session_id.empty() || session.session_id != settings.native_session_id) return nullptr;
		const bool local = session.process_execution_host_id == "local" && settings.execution_host_id == "local";
		if (local)
		{
			if (!uam::paths::PathFromUtf8(settings.workspace_directory).is_absolute()) return nullptr;
		}
		else
		{
			// Only the invocation captured by the shared owner/connection fences may select remote input.
			if (!session.saved_image_retry || !session.saved_image_retry->remote_scope) return nullptr;
			const auto& permission = *session.saved_image_retry;
			const auto& scope = *permission.remote_scope;
			if (scope.host.id.empty() || scope.host.id == "local" || scope.host.transport != "ssh" || scope.host.ssh_alias.empty() ||
			    permission.execution_host_id != scope.host.id || settings.execution_host_id != scope.host.id || session.process_execution_host_id != scope.host.id ||
			    scope.native_session_id != settings.native_session_id || scope.runtime_session_id != session.session_id || scope.process_id != process ||
			    scope.proxy_token.empty() || scope.proxy_token != session.remote_output_delivery_token || scope.proxy_protocol < 3 ||
			    scope.proxy_protocol != session.remote_runner_protocol_version || scope.proxy_protocol != scope.host.runner_protocol_version ||
			    !uam::execution_hosts::IsAbsoluteRemotePath(scope.host.platform, settings.workspace_directory)) return nullptr;
		}
		// ACP's negotiated image capability is the native contract; model names imply no vision guarantee.
		nlohmann::json owner = {{"protocol", session.protocol_kind}, {"image", session.prompt_image_supported},
		    {"sessionId", session.session_id}, {"savedNativeSessionId", settings.native_session_id},
		    {"processId", process}, {"executionHostId", session.process_execution_host_id},
		    {"workspace", settings.workspace_directory}, {"agentVersion", session.agent_version},
		    {"currentModelId", session.current_model_id}, {"currentModeId", session.current_mode_id},
		    {"modelId", settings.model_id}, {"reasoningEffort", settings.reasoning_effort},
		    {"approvalMode", settings.approval_mode}, {"serviceTier", settings.service_tier},
		    {"serviceTierExplicit", settings.service_tier_explicit}};
		if (!local)
		{
			owner["remoteOwnedRetry"] = true;
			owner["executionPlatform"] = session.saved_image_retry->remote_scope->host.platform;
		}
		return owner;
	}

	std::string RebuiltGeminiSessionNoteText(bool has_tool_calls, bool is_subagent)
	{
		std::string note_text = "Note: This session was rebuilt from UAM's source of truth.";
		if (has_tool_calls)
		{
			note_text += " Tool execution details and results were preserved but subagent conversations were stripped to reduce context.";
		}
		if (is_subagent)
		{
			note_text += " This is a subagent session extracted from a parent conversation.";
		}
		note_text += " The main conversation thread contains the essential information.";
		return note_text;
	}

	JsonValue TextContentJson(std::string_view text)
	{
		JsonValue content_json = uam::json::Object();
		uam::json::SetString(content_json, "text", text);
		return content_json;
	}

	JsonValue GeminiThoughtsJson(std::string_view thoughts)
	{
		JsonValue thoughts_json = uam::json::Array();
		std::istringstream thought_stream{std::string(thoughts)};
		std::string line;
		while (std::getline(thought_stream, line))
		{
			if (!line.empty())
			{
				JsonValue thought_json = uam::json::Object();
				uam::json::SetString(thought_json, "text", line);
				uam::json::PushValue(thoughts_json, std::move(thought_json));
			}
		}
		return thoughts_json;
	}

	JsonValue GeminiToolCallsJson(const std::vector<ToolCall>& tool_calls, std::string_view timestamp)
	{
		JsonValue tool_calls_json = uam::json::Array();
		for (const ToolCall& tool_call : tool_calls)
		{
			JsonValue tool_call_json = uam::json::Object();
			uam::json::SetString(tool_call_json, "id", tool_call.id);
			uam::json::SetString(tool_call_json, "name", tool_call.name);
			uam::json::SetString(tool_call_json, "status", tool_call.status);
			uam::json::SetString(tool_call_json, "timestamp", timestamp);

			const auto parsed_args = ParseJson(tool_call.args_json);
			uam::json::SetValue(tool_call_json, "args", parsed_args ? *parsed_args : uam::json::Object());
			uam::json::SetValue(tool_call_json, "result", TextContentJson(tool_call.result_text));
			uam::json::PushValue(tool_calls_json, std::move(tool_call_json));
		}
		return tool_calls_json;
	}

} // namespace

bool GeminiCliProviderRuntime::SupportsSavedImageRetry(const uam::AcpSessionState& session) const
{
	// Cold eligibility precedes initialize. Known current-process rejection must
	// refuse before row admission; a new process still needs fresh negotiation.
	return session.protocol_kind == "gemini-acp" && (!session.running || !session.initialized || session.prompt_image_supported);
}

bool GeminiCliProviderRuntime::SupportsRemoteOwnedImageRetry(const uam::AcpSessionState& session) const
{
	// Opt-in is capability only; the shared invocation scope still owns process and delivery authority.
	return session.protocol_kind == "gemini-acp" && session.running && session.initialized && session.session_ready && session.prompt_image_supported;
}

SavedImageRetryPreflight GeminiCliProviderRuntime::PrepareSavedImageRetry(uam::AcpSessionState& session,
    const ChatSession& settings, const std::vector<MessageAttachment>& images, std::string& error) const
{
	if (images.empty() || images.size() > 32 || GeminiSavedImageOwner(session, settings).is_null())
	{
		error = session.saved_image_retry && session.saved_image_retry->remote_scope
		    ? "Gemini saved-image Retry requires current negotiated ACP image support in its same ready SSH native session. The prompt was not sent."
		    : "Gemini saved-image Retry requires current negotiated ACP image support in its ready local native session. The prompt was not sent.";
		return SavedImageRetryPreflight::Rejected;
	}
	error.clear();
	return SavedImageRetryPreflight::Ready;
}

nlohmann::json GeminiCliProviderRuntime::SavedImageRetryMetadata(const uam::AcpSessionState& session,
    const ChatSession& settings) const
{
	return GeminiSavedImageOwner(session, settings);
}

bool GeminiCliProviderRuntime::EncodePreparedSavedImageRetry(nlohmann::json& request,
    const nlohmann::json& prepared, std::string& error, const std::vector<MessageAttachment>& images,
    const nlohmann::json& metadata) const
try
{
	const auto text = [](const nlohmann::json& object, const char* key) -> std::string
	{
		const auto found = object.find(key);
		return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
	};
	const auto fail = [&error]()
	{
		error = "Gemini could not retain complete negotiated image input. The prompt was not sent.";
		return false;
	};
	const auto limit = uam::platform::kAsyncInputMaxQueuedBytes;
	if (images.empty() || images.size() > 32 || !request.is_object() || text(request, "jsonrpc") != "2.0" ||
	    text(request, "method") != "session/prompt" || !request.contains("id") || !request["id"].is_number_integer() || request["id"].get<std::int64_t>() <= 0 ||
	    !metadata.is_object() || text(metadata, "protocol") != "gemini-acp" || metadata.value("image", false) != true ||
	    text(metadata, "processId").empty() || text(metadata, "executionHostId").empty() ||
	    text(metadata, "sessionId").empty() || text(metadata, "sessionId") != text(metadata, "savedNativeSessionId") ||
	    !request.contains("params") || !request["params"].is_object() || !prepared.is_object() || !prepared.contains("message") || !prepared["message"].is_object() ||
	    !prepared["message"].contains("content") || !prepared["message"]["content"].is_array()) return fail();
	const bool remote = text(metadata, "executionHostId") != "local";
	if (remote && (!metadata.value("remoteOwnedRetry", false) ||
	    !uam::execution_hosts::IsAbsoluteRemotePath(text(metadata, "executionPlatform"), text(metadata, "workspace")))) return fail();
	const auto& params = request["params"];
	const auto& content = prepared["message"]["content"];
	if (text(params, "sessionId") != text(metadata, "sessionId") ||
	    !params.contains("prompt") || !params["prompt"].is_array() || params["prompt"].size() != 1 ||
	    !params["prompt"][0].is_object() || text(params["prompt"][0], "type") != "text" || !params["prompt"][0].contains("text") || !params["prompt"][0]["text"].is_string() ||
	    content.size() != images.size() + 1 || !content[0].is_object() || text(content[0], "type") != "text" || !content[0].contains("text") || !content[0]["text"].is_string()) return fail();
	const auto& prepared_text = content[0]["text"].get_ref<const std::string&>();
	if (prepared_text.size() > limit) return fail();
	std::string labels;
	std::size_t total_bytes = 0;
	for (std::size_t index = 0; index < images.size(); ++index)
	{
		if (images[index].kind != "image" || images[index].name.size() > limit ||
		    (remote && (images[index].source != AttachmentSource::OwnedNativeAsset ||
		        images[index].origin_execution_host_id != text(metadata, "executionHostId")))) return fail();
		const std::string prefix = "\nImage " + std::to_string(index + 1) + " (native image-block order): ";
		const std::string name = nlohmann::json(images[index].name).dump();
		if (prefix.size() > limit - labels.size() || name.size() > limit - labels.size() - prefix.size()) return fail();
		labels += prefix + name;
		const auto& part = content[index + 1];
		if (!part.is_object() || part.size() != 2 || text(part, "type") != "image" || !part.contains("source") || !part["source"].is_object()) return fail();
		const auto& source = part["source"];
		if (source.size() != 3 || text(source, "type") != "base64" || !source.contains("data") || !source["data"].is_string()) return fail();
		const auto& data = source["data"].get_ref<const std::string&>();
		const auto mime = text(source, "media_type");
		std::string decoded;
		if (data.empty() || data.size() > limit || (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp") ||
		    !uam::base64::Decode(data, decoded) || decoded.empty() || decoded.size() > limit - total_bytes || uam::base64::Encode(decoded) != data) return fail();
		total_bytes += decoded.size();
	}
	if (!prepared_text.ends_with(labels)) return fail();
	const auto body = prepared_text.substr(0, prepared_text.size() - labels.size());
	const auto& original_text = params["prompt"][0]["text"].get_ref<const std::string&>();
	if (!original_text.ends_with(body)) return fail();
	// The owned worker already verified/decoded the originals. Preserve the ordinary ACP
	// builder's context and append its exact captured-name labels and native image blocks.
	auto encoded = request;
	encoded["params"]["prompt"][0]["text"] = original_text + labels;
	uam::structured_attachments::AppendPreparedAcpImages(encoded, prepared);
	if (encoded["params"]["prompt"].size() != images.size() + 1 || encoded.dump().size() >= limit) return fail();
	request = std::move(encoded);
	error.clear();
	return true;
}
catch (...)
{
	error = "Gemini image input or its negotiated session evidence is invalid. The prompt was not sent.";
	return false;
}

const ProviderCliPolicy* GeminiCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kGeminiCli,
		.npm_package = "@google/gemini-cli",
		.fallback_title = "Gemini CLI",
		.executable_name = "gemini",
		.version_probe_command = "gemini --version",
		.homebrew_package = "gemini-cli",
		.preferred_version = "latest",
		.fallback_version = "0.55.1",
		.minimum_version = "0.55.1",
		.version_policy = ProviderCliVersionPolicy::MinimumSemver,
		.verified_at = "2026-08-27",
	};
	return &policy;
}

bool GeminiCliProviderRuntime::OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const uam::acp_detail::AcpResponseFailureDetails& details) const
{
	using namespace uam::acp_detail;
	if (!TextContainsAnyCaseInsensitive(details.failure.message + "\n" + details.error_data,
	        {"invalid session identifier", "use --list-sessions"})) return false;
	return RetrySessionNewAfterInvalidLoad(app, session, chat, details);
}

bool GeminiCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& message, const CefRefPtr<CefBrowser>&) const
{
	if (session.managed_initial_owner && uam::nlohmann_json::StringViewOrEmpty(message, "method") == uam::acp_methods::kSessionUpdate)
	{
		const auto* params = uam::nlohmann_json::FindObjectField(message, "params");
		const auto* update = params ? uam::nlohmann_json::FindObjectField(*params, "update") : nullptr;
		const auto type = update ? uam::nlohmann_json::StringViewOrEmpty(*update, "sessionUpdate") : std::string_view{};
		const auto native = params ? uam::nlohmann_json::StringViewOrEmpty(*params, "sessionId") : std::string_view{};
		const auto* content = update ? uam::nlohmann_json::FindObjectField(*update, "content") : nullptr;
		// Both qualified native versions emit approval-mode changes as this text
		// notification, not current_mode_update. It can revoke proof but never mint it.
		const bool native_mode_signal = type == "agent_message_chunk" && content && uam::nlohmann_json::StringViewOrEmpty(*content, "text").starts_with("[MODE_UPDATE]");
		if (native.empty() || native != session.session_id || native != chat.native_session_id || native_mode_signal || type == "current_mode_update" || type == "config_option_update" || type == "current_model_update")
		{
			// This initial-only tranche cannot prove a configuration transition. Even
			// restoration to matching projected fields cannot repair the old proof.
			uam::AgentRunScheduler::InitialGeminiExecutionObserved(app, session, chat, {}, {}, {}, {});
		}
	}
	return false; // Retain ordinary ACP projection and the initial one-shot behavior.
}

/// <summary>Observe raw initial binding evidence and current-process prompt usage without native-history inference.</summary>
bool GeminiCliProviderRuntime::OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const std::string& method, const std::string& request_id, const nlohmann::json& result) const
{
	if (session.managed_initial_owner && method == uam::acp_methods::kSessionNew)
	{
		const auto* models = uam::nlohmann_json::FindObjectField(result, "models");
		const auto* modes = uam::nlohmann_json::FindObjectField(result, "modes");
		const auto* options = uam::nlohmann_json::FindField(result, "configOptions");
		const auto model = models ? uam::nlohmann_json::StringViewOrEmpty(*models, "currentModelId") : std::string_view{};
		const auto* advertised_models = models ? uam::nlohmann_json::FindArrayField(*models, "availableModels") : nullptr;
		const auto* advertised_modes = modes ? uam::nlohmann_json::FindArrayField(*modes, "availableModes") : nullptr;
		// These exact versions' newSession/initialize implementations were inspected.
		// Minimum-semver launch support does not qualify future native binding proof.
		const bool qualified_native = session.agent_name == "gemini-cli" && (session.agent_version == "0.55.1" || session.agent_version == "0.64.0");
		const bool advertised = advertised_models && advertised_modes && advertised_models->size() <= 128 && advertised_modes->size() <= 32 && std::ranges::count_if(*advertised_models, [model](const auto& entry) { return uam::nlohmann_json::StringViewOrEmpty(entry, "modelId") == model; }) == 1 && std::ranges::count_if(*advertised_modes, [](const auto& entry) { return uam::nlohmann_json::StringViewOrEmpty(entry, "id") == uam::approval_modes::kPlanApprovalMode; }) == 1;
		// Shared projection may normalize strings or override models/modes from
		// configOptions. Neither fallback is native proof for this narrow route.
		const bool unambiguous = qualified_native && advertised && (!options || (options->is_array() && options->empty()));
		uam::AgentRunScheduler::InitialGeminiExecutionObserved(app, session, chat, request_id,
		    unambiguous ? uam::nlohmann_json::StringViewOrEmpty(result, "sessionId") : std::string_view{},
		    model,
		    modes ? uam::nlohmann_json::StringViewOrEmpty(*modes, "currentModeId") : std::string_view{});
	}
	else if (session.managed_initial_owner && (method == uam::acp_methods::kSessionSetMode || method == uam::acp_methods::kSessionSetModel || method == uam::acp_methods::kSessionSetConfigOption))
	{
		uam::AgentRunScheduler::InitialGeminiExecutionObserved(app, session, chat, {}, {}, {}, {});
	}
	if (method != uam::acp_methods::kSessionPrompt) return false;
	if (session.managed_initial_owner)
		uam::AgentRunScheduler::InitialGeminiResultObserved(app, session, chat, request_id, ManagedGeminiCompletedModel(result));
	uam::AcpTokenUsageState& usage = session.provider_usage.token_usage;
	if (usage.session_id != session.session_id) usage = {};
	if (session.session_id.empty() || session.session_id.size() > 512) return false;
	const nlohmann::json* tokens = uam::nlohmann_json::FindObjectField(result, "usage");
	const bool standard = tokens != nullptr;
	if (!tokens)
	{
		const nlohmann::json* meta = uam::nlohmann_json::FindObjectField(result, "_meta");
		const nlohmann::json* quota = meta ? uam::nlohmann_json::FindObjectField(*meta, "quota") : nullptr;
		tokens = quota ? uam::nlohmann_json::FindObjectField(*quota, "token_count") : nullptr;
	}
	// A completed prompt without counts must not leave a prior prompt labeled as the last turn.
	usage.last_available = false;
	if (!tokens) return false;
	const std::function<std::optional<std::int64_t>(const char*, bool)> count = [&](const char* key, bool required) -> std::optional<std::int64_t>
	{
		const nlohmann::json* field = uam::nlohmann_json::FindField(*tokens, key);
		if (!field || field->is_null()) return required ? std::nullopt : std::optional<std::int64_t>{0};
		if (!field->is_number_integer()) return std::nullopt;
		if (field->is_number_unsigned() && field->get<std::uint64_t>() > static_cast<std::uint64_t>(uam::usage::kMaxExactTokenCount)) return std::nullopt;
		const std::int64_t parsed = field->get<std::int64_t>();
		return parsed >= 0 && parsed <= uam::usage::kMaxExactTokenCount ? std::optional<std::int64_t>{parsed} : std::nullopt;
	};
	const std::optional<std::int64_t> input = count(standard ? "inputTokens" : "input_tokens", true);
	const std::optional<std::int64_t> output = count(standard ? "outputTokens" : "output_tokens", true);
	const std::optional<std::int64_t> cached = standard ? count("cachedReadTokens", false) : std::optional<std::int64_t>{0};
	const std::optional<std::int64_t> thought = standard ? count("thoughtTokens", false) : std::optional<std::int64_t>{0};
	if (!input || !output || !cached || !thought || *cached > *input || *output > uam::usage::kMaxExactTokenCount - *input ||
	    *thought > uam::usage::kMaxExactTokenCount - *input - *output) return false;
	const std::int64_t total = *input + *output + *thought;
	if (standard)
	{
		const std::optional<std::int64_t> reported = count("totalTokens", true);
		// Gemini candidates exclude reasoning; accept a total that either includes or excludes it.
		if (!reported || (*reported != *input + *output && *reported != total)) return false;
	}
	if (total > uam::usage::kMaxExactTokenCount - usage.total.total_tokens) return false;
	uam::AcpTokenUsageBreakdownState last;
	last.input_tokens = *input;
	last.cached_input_tokens = *cached;
	last.output_tokens = *output + *thought;
	last.reasoning_output_tokens = *thought;
	last.total_tokens = total;
	usage.session_id = session.session_id;
	usage.available = true;
	usage.last_available = true;
	usage.last = last;
	usage.total.input_tokens += last.input_tokens;
	usage.total.cached_input_tokens += last.cached_input_tokens;
	usage.total.output_tokens += last.output_tokens;
	usage.total.reasoning_output_tokens += last.reasoning_output_tokens;
	usage.total.total_tokens += last.total_tokens;
	usage.updated_at_ms = uam::time::SystemEpochMillisecondsNow();
	return false;
}

const char* GeminiCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kGeminiCli;
}


std::vector<std::string> GeminiCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	return uam::provider_runtime_internal::BuildInteractiveArgv(profile, chat, uam::provider_runtime_internal::MergeProviderSettings(profile, settings));
}

MessageRole GeminiCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> GeminiCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path&, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	return LoadGeminiJsonHistoryForRuntime(native_history_chats_dir, profile, options);
}

bool GeminiCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool GeminiCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return false;
}

bool GeminiCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool GeminiCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return true;
}

std::string GeminiCliProviderRuntime::GenerateSessionUUID() const
{
	return PlatformServicesFactory::Instance().process_service.GenerateUuid();
}

std::string GeminiCliProviderRuntime::BuildSessionFilename(const ChatSession& chat) const
{
	return chat.native_session_id + ".json";
}

std::string GeminiCliProviderRuntime::NativeTypeFromRole(MessageRole role) const
{
	switch (role)
	{
	case MessageRole::User:
		return "user";
	case MessageRole::Assistant:
		return "gemini";
	case MessageRole::System:
		return "info";
	default:
		return "info";
	}
}

std::filesystem::path GeminiCliProviderRuntime::GetNativeSessionDirectory(const std::filesystem::path& workspace_path) const
{
	const auto tmp_dir = AppPaths::ResolveGeminiProjectTmpDir(workspace_path);
	if (!tmp_dir)
	{
		return {};
	}
	return *tmp_dir / "chats";
}

bool GeminiCliProviderRuntime::RebuildNativeSessionFile(const ProviderProfile&, const ChatSession& chat, const std::filesystem::path& workspace_path) const
{
	if (!uam::chat_ids::IsSafeStorageChatId(chat.native_session_id) || workspace_path.empty())
	{
		return false;
	}

	const fs::path chats_dir = GetNativeSessionDirectory(workspace_path);
	if (chats_dir.empty())
	{
		return false;
	}

	std::error_code error;
	if (!uam::paths::CreateDirectoriesNoThrow(chats_dir, &error))
	{
		return false;
	}

	const bool has_tool_calls = std::ranges::any_of(chat.messages, [](const Message& message) { return !message.tool_calls.empty(); });
	const bool is_subagent = !chat.parent_chat_id.empty();

	JsonValue root = uam::json::Object();
	uam::json::SetString(root, "sessionId", chat.native_session_id);
	uam::json::SetString(root, "startTime", chat.created_at);
	uam::json::SetString(root, "lastUpdated", chat.updated_at);

	JsonValue messages_json = uam::json::Array();
	if (has_tool_calls || is_subagent)
	{
		JsonValue note_message_json = uam::json::Object();
		uam::json::SetString(note_message_json, "id", GenerateSessionUUID());
		uam::json::SetString(note_message_json, "timestamp", chat.created_at);
		uam::json::SetString(note_message_json, "type", "info");

		uam::json::SetValue(note_message_json, "content", TextContentJson(RebuiltGeminiSessionNoteText(has_tool_calls, is_subagent)));
		uam::json::SetValue(note_message_json, "thoughts", uam::json::Array());
		uam::json::SetValue(note_message_json, "toolCalls", uam::json::Array());
		uam::json::PushValue(messages_json, std::move(note_message_json));
	}

	for (const Message& message : chat.messages)
	{
		JsonValue message_json = uam::json::Object();
		uam::json::SetString(message_json, "id", GenerateSessionUUID());
		uam::json::SetString(message_json, "timestamp", message.created_at);
		uam::json::SetString(message_json, "type", NativeTypeFromRole(message.role));

		uam::json::SetValue(message_json, "content", TextContentJson(message.content));
		uam::json::SetValue(message_json, "thoughts", GeminiThoughtsJson(message.thoughts));
		uam::json::SetValue(message_json, "toolCalls", GeminiToolCallsJson(message.tool_calls, message.created_at));

		uam::json::PushValue(messages_json, std::move(message_json));
	}

	uam::json::SetValue(root, "messages", std::move(messages_json));
	const std::string session_filename = BuildSessionFilename(chat);
	return uam::io::WriteTextFile(chats_dir / session_filename, SerializeJson(root));
}

const IProviderRuntime& GetGeminiCliProviderRuntime()
{
	static const GeminiCliProviderRuntime runtime;
	return runtime;
}

ProviderDiscoveryResult GeminiCliProviderRuntime::DiscoverChatSources(const ProviderProfile&) const
{
	ProviderDiscoveryResult result;
	result.sources = DiscoverGeminiTmpChatSources(&result.error);
	return result;
}

std::vector<std::string> GeminiCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"gemini"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);
	argv.push_back("-p");
	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> GeminiCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"gemini", "--acp"};
	const std::string approval_mode = uam::approval_modes::GeminiProviderApprovalModeFromAppModeId(uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode));
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--approval-mode", approval_mode);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	uam::computer_use::AppendGeminiMcpLaunchArguments(argv, chat);
	return argv;
}

std::string GeminiCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
	return normalized == uam::approval_modes::kAcceptEditsApprovalMode
	    ? std::string(uam::approval_modes::kGeminiAcpAutoEditApprovalMode) : std::string(normalized);
}
