#include "common/provider/codex/cli/codex_cli_provider_runtime.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/approval_modes.h"
#include "common/paths/workspace_root.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/provider/codex/cli/codex_acp_message_handlers.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/io_utils.h"

#include <unordered_set>
#include "common/utils/string_utils.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"

namespace
{
	void FinishCodexSteer(uam::AppState& app, uam::AcpSessionState& session,
	    ChatSession& chat, const std::string& request_id, const std::string& error)
	{
		const std::unordered_map<std::string, uam::AcpPendingSteerState>::iterator pending = session.pending_steer_requests.find(request_id);
		if (pending == session.pending_steer_requests.end()) return;
		const int index = pending->second.user_message_index;
		if (!error.empty())
		{
			if (index >= 0 && index < static_cast<int>(chat.messages.size()) && chat.messages[static_cast<std::size_t>(index)].role == MessageRole::User)
			{
				chat.messages[static_cast<std::size_t>(index)].interrupted = true;
				(void)uam::acp_detail::SaveChatQuietly(app, chat);
			}
			if (pending->second.turn_serial == session.turn_serial) session.last_error = error;
		}
		session.pending_steer_requests.erase(pending);
	}

	struct CodexModelParseOptions
	{
		bool skip_hidden_field = true;
		bool allow_default_non_list_visibility = false;
	};

	struct ParsedCodexModelEntry
	{
		uam::AcpModelState model;
		bool is_default = false;
	};

	std::optional<ParsedCodexModelEntry> ParseCodexModelEntry(const nlohmann::json& model, const CodexModelParseOptions& options = {})
	{
		if (!model.is_object())
		{
			return std::nullopt;
		}
		const nlohmann::json* hidden = uam::nlohmann_json::FindField(model, "hidden");
		if (options.skip_hidden_field && hidden != nullptr && hidden->is_boolean() && hidden->get<bool>())
		{
			return std::nullopt;
		}

		ParsedCodexModelEntry parsed;
		const nlohmann::json* is_default = uam::nlohmann_json::FindField(model, "isDefault");
		parsed.is_default = is_default != nullptr && is_default->is_boolean() && is_default->get<bool>();
		parsed.model.id = uam::nlohmann_json::TrimmedStringValue(model, {"id", "model", "slug", "modelId"});
		if (parsed.model.id.empty())
		{
			return std::nullopt;
		}

		const std::string visibility = uam::nlohmann_json::TrimmedStringValue(model, {"visibility"});
		if (!visibility.empty() && visibility != "list" && !(options.allow_default_non_list_visibility && parsed.is_default))
		{
			return std::nullopt;
		}

		parsed.model.name = uam::nlohmann_json::TrimmedStringValue(model, {"displayName", "display_name", "name"});
		if (parsed.model.name.empty())
		{
			parsed.model.name = parsed.model.id;
		}
		parsed.model.description = uam::nlohmann_json::TrimmedStringValue(model, {"description"});
		parsed.model.default_reasoning_effort = uam::codex::NormalizeReasoningEffort(uam::nlohmann_json::TrimmedStringValue(model, {"defaultReasoningEffort", "default_reasoning_effort", "defaultReasoningLevel", "default_reasoning_level"}));
		parsed.model.supported_reasoning_efforts = uam::acp_models::UniqueStringArrayValue(model, {"supportedReasoningEfforts", "supported_reasoning_levels"}, uam::codex::NormalizeReasoningEffort);
		parsed.model.additional_speed_tiers = uam::acp_models::UniqueStringArrayValue(model, {"additionalSpeedTiers", "additional_speed_tiers", "serviceTiers", "service_tiers"}, uam::acp_models::NormalizeModelServiceTier);
		return parsed;
	}
nlohmann::json BuildModelListRequest(int request_id, const std::string& cursor = {})
{
	nlohmann::json params = nlohmann::json::object();
	if (!cursor.empty()) params["cursor"] = cursor;
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kModelList, std::move(params));
}

constexpr auto kCodexPromptMarkers = std::to_array<std::string_view>({"\xE2\x80\xBA", "> "});
constexpr auto kCodexPromptCueTexts = std::to_array<std::string_view>({"Send", "message", "for shortcuts"});
}

namespace uam::acp_detail
{

bool SendDeferredCodexInterruptIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	if (!session.cancel_requested || session.cancel_request_id != 0 || session.session_id.empty() || session.codex_turn_id.empty())
	{
		return false;
	}

	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
	const int request_id = session.next_request_id++;
	std::string method;
	const nlohmann::json message = runtime.OnAcpBuildCancel(session, request_id, method);
	if (message.is_null() || message.empty() || method != uam::acp_methods::kTurnInterrupt)
	{
		return false;
	}

	session.pending_request_methods[request_id] = method;
	session.cancel_request_id = request_id;
	if (!WriteAcpMessage(session, message))
	{
		session.pending_request_methods.erase(request_id);
		session.cancel_request_id = 0;
		InvalidateAcpTransport(app, session, chat, session.last_error);
		return false;
	}
	return true;
}

} // namespace uam::acp_detail

bool CodexCliProviderRuntime::RecentOutputIndicatesInputPrompt(std::string_view recent_output) const
{
	const std::string stripped = uam::RecentTerminalPromptScanText(recent_output);
	if (stripped.empty())
	{
		return false;
	}

	return uam::strings::ContainsAny(stripped, kCodexPromptMarkers) && uam::strings::ContainsAny(stripped, kCodexPromptCueTexts);
}

const ProviderCliPolicy* CodexCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kCodexCli,
		.npm_package = "@openai/codex",
		.fallback_title = "Codex CLI",
		.executable_name = "codex",
		.version_probe_command = "codex --version",
		.homebrew_package = "codex",
		.homebrew_cask = true,
		.preferred_version = "latest",
		.fallback_version = "0.148.0",
		.verified_at = "2026-08-27",
	};
	return &policy;
}

const char* CodexCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCodexCli;
}


nlohmann::json CodexCliProviderRuntime::ReadLocalModelCatalog() const
{
	nlohmann::json models_json = nlohmann::json::array();
	const nlohmann::json cache = nlohmann::json::parse(uam::io::ReadTextFile(uam::codex::CodexHomePath() / "models_cache.json"), nullptr, false);
	if (!cache.is_object())
	{
		return models_json;
	}

	const nlohmann::json* models = uam::nlohmann_json::FindArrayField(cache, "models");
	if (models == nullptr)
	{
		return models_json;
	}

	std::unordered_set<std::string> seen_model_ids;
	CodexModelParseOptions parse_options;
	parse_options.skip_hidden_field = false;
	parse_options.allow_default_non_list_visibility = false;
	for (const nlohmann::json& model : *models)
	{
		const std::optional<ParsedCodexModelEntry> parsed = ParseCodexModelEntry(model, parse_options);
		if (!parsed || parsed->model.id.empty() || !seen_model_ids.insert(parsed->model.id).second)
		{
			continue;
		}

		models_json.push_back({
		    {"id", parsed->model.id},
		    {"name", parsed->model.name},
		    {"description", parsed->model.description},
		    {"defaultReasoningEffort", parsed->model.default_reasoning_effort},
		    {"supportedReasoningEfforts", parsed->model.supported_reasoning_efforts},
		    {"additionalSpeedTiers", parsed->model.additional_speed_tiers},
		});
	}

	return models_json;
}

std::string CodexCliProviderRuntime::ResolveInteractiveResumeId(const uam::AppState& app, const ChatSession& chat) const
{
	const std::string resolved_session_id = uam::ResolvedNativeSessionIdForChat(app, chat);
	return uam::codex::ValidThreadIdOrEmpty(resolved_session_id.empty() ? chat.native_session_id : resolved_session_id);
}

std::vector<std::string> CodexCliProviderRuntime::SnapshotInteractiveSessionIds() const
{
	return uam::codex::ReadSessionIndexIds();
}

std::string CodexCliProviderRuntime::DiscoverInteractiveSessionId(const std::vector<std::string>& before, const std::filesystem::path& workspace) const
{
	return uam::codex::PickNewSessionId(before, workspace);
}


std::vector<std::string> CodexCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv;
	const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	if (profile.supports_resume && !resume_id.empty())
	{
		uam::provider_runtime_internal::AppendLiteralArgs(argv, {"codex", "resume", "--no-alt-screen"});
		argv.push_back(resume_id);
	}
	else
	{
		argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "codex --no-alt-screen");
	}

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-m", chat.model_id);
	const std::string reasoning_effort = uam::codex::NormalizeReasoningEffort(chat.reasoning_effort);
	const std::string service_tier = uam::codex::NormalizeServiceTier(chat.service_tier);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-c", reasoning_effort.empty() ? "" : "model_reasoning_effort=\"" + reasoning_effort + "\"");
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-c", !chat.service_tier_explicit ? "" : service_tier.empty() ? "service_tier=null" : "service_tier=\"" + service_tier + "\"");

	uam::provider_runtime_internal::AppendArgs(argv, uam::provider_runtime_internal::BuildProviderFlagsArgv(provider_settings));
	return argv;
}

MessageRole CodexCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

void CodexCliProviderRuntime::ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json&) const
{
	if (tool.name != "spawn_agent" && tool.name != "multi_agent_v1.spawn_agent" && tool.name != "functions.spawn_agent") return;
	tool.is_sub_agent = true;
	const nlohmann::json result = nlohmann::json::parse(tool.result_text, nullptr, false);
	const std::string child_id = uam::codex::ValidThreadIdOrEmpty(uam::nlohmann_json::StringViewOrEmpty(result, "agent_id"));
	if (child_id.empty()) return;
	if (tool.sub_agent_id != child_id) tool.sub_agent_title.clear();
	tool.sub_agent_id = child_id;
	const std::string_view nickname = uam::nlohmann_json::StringViewOrEmpty(result, "nickname");
	if (!nickname.empty()) tool.sub_agent_title = nickname;
}

std::vector<ChatSession> CodexCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	(void)profile;
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

void CodexCliProviderRuntime::NormalizeLoadedNativeSessionId(ChatSession& chat) const
{
	chat.native_session_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
}

bool CodexCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}


std::vector<std::string> CodexCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"codex", "exec"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add read-only args for worker mode
	constexpr const char* kCodexReadOnlyWorkerArgs[] = {
	    "--ignore-user-config", "--ignore-rules", "--json", "--color", "never",
	    "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
	    "-c", "model_reasoning_effort=\"low\"",
	};
	for (const char* arg : kCodexReadOnlyWorkerArgs)
	{
		argv.push_back(arg);
	}
	
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-m", model_id);

	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> CodexCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"codex"};
	uam::computer_use::AppendCodexMcpLaunchArguments(argv, chat);
	uam::provider_runtime_internal::AppendLiteralArgs(argv, {"app-server", "--listen", "stdio://"});
	return argv;
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	(void)session;
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kInitialize,
	                                  {
	                                      {"clientInfo", uam::acp_request_defaults::ClientInfo()},
	                                      {"capabilities",
	                                       {
	                                           {"experimentalApi", true},
	                                       }},
	                                  });
}

bool CodexCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const
{
	using namespace uam::acp_detail;
	if (uam::nlohmann_json::FindField(message, "method") == nullptr) return false;
	const std::string method = JsonDiagnosticStringValue(message, "method");
	try
	{
		HandleCodexMessage(app, session, chat, message, browser);
		MarkAcpRuntimeActivity(session);
	}
	catch (const std::exception& ex)
	{
		const std::string error_message = std::string("Codex app-server message handling failed: ") + ex.what();
		AppendAcpDiagnostic(session, "parse", "codex_message_parse_error", method, "", false, 0,
		                    error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		InvalidateAcpTransport(app, session, chat, error_message);
	}
	return true;
}

void CodexCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	using namespace uam::acp_detail;
	session.codex_model_discovery = {};
	session.agent_name = "codex";
	session.agent_title = "Codex";
	if (result.is_object())
	{
		session.agent_version = JsonDiagnosticStringValue(result, "userAgent");
	}
	session.load_session_supported = true;
	if (!WriteAcpMessage(session, uam::acp_json_rpc::Notification(uam::acp_methods::kInitialized, nullptr))) return;
	const int model_list_id = NextAcpRequestId(session, uam::acp_methods::kModelList);
	if (!WriteAcpMessage(session, BuildModelListRequest(model_list_id)))
	{
		session.pending_request_methods.erase(model_list_id);
		return;
	}
	const int rate_limits_id = NextAcpRequestId(session, uam::acp_methods::kAccountRateLimitsRead);
	if (!WriteAcpMessage(session, nlohmann::json{{"jsonrpc", uam::acp_json_rpc::kVersion}, {"id", rate_limits_id}, {"method", uam::acp_methods::kAccountRateLimitsRead}}))
	{
		session.pending_request_methods.erase(rate_limits_id);
	}
}

bool CodexCliProviderRuntime::OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const uam::acp_detail::AcpResponseFailureDetails& details) const
{
	using namespace uam::acp_detail;
	const AcpFailureDetails& failure = details.failure;
	const std::string& detail_text = details.detail_text;
	const std::string& formatted_error = details.formatted_error;
	if (failure.method == "turn/steer")
	{
		FinishCodexSteer(app, session, chat, failure.request_id, formatted_error);
		return true;
	}
	if (failure.method == uam::acp_methods::kModelList)
	{
		if (app.provider_model_catalog != nullptr)
		{
			app.provider_model_catalog->RememberRefreshFailure(session.provider_id,
			    formatted_error, DiscoveryWorkspace(app, chat), chat.execution_host_id);
		}
		// Catalog failures must not terminate an independent chat turn.
		session.codex_model_discovery = {};
		StopBackgroundModelDiscovery(app, session);
		return true;
	}
	if (failure.method == uam::acp_methods::kAccountRateLimitsRead)
	{
		return true;
	}
	if (failure.method == uam::acp_methods::kThreadResume && failure.has_code && failure.code == -32600 && uam::codex::ErrorLooksLikeInvalidThreadId(failure.message) && !session.codex_resume_fallback_attempted)
	{
		if (!ClearSavedAcpResumeId(app, session, chat)) return true;
		session.codex_resume_fallback_attempted = true;
		session.session_setup_request_id = 0;
		session.session_id.clear();
		session.codex_thread_id.clear();
		session.provider_usage.token_usage = uam::AcpTokenUsageState{};
		SyncResolvedNativeSessionIdForChat(app, chat, {});
		AppendAcpDiagnostic(session, "response", "codex_invalid_resume_id_retry_start", failure.method, failure.request_id, failure.has_code, failure.code, "Codex rejected the stored thread id. Starting a new thread instead.", detail_text);

		const std::filesystem::path workspace_root = uam::paths::ResolveWorkspaceRootPath(app, chat);
		const std::string cwd = AcpWorkingDirectoryString(workspace_root);
		const int retry_id = session.next_request_id++;
		std::string retry_method;
		const nlohmann::json retry_request = OnAcpBuildSetupRequest(retry_id, chat, cwd, false, retry_method);
		session.pending_request_methods[retry_id] = retry_method;
		session.session_setup_request_id = retry_id;
		session.lifecycle_state = kAcpLifecycleStarting;
		if (!WriteAcpMessage(session, retry_request))
		{
			session.pending_request_methods.erase(retry_id);
			session.session_setup_request_id = 0;
			RecoverDisconnectedRemoteAcpTransport(
			    app, session, chat,
			    uam::strings::NonEmptyOrFallback(session.last_error, formatted_error));
		}
		return true;
	}
	return false;
}

bool CodexCliProviderRuntime::OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const std::string& method, const std::string& request_id, const nlohmann::json& result) const
{
	using namespace uam;
	using namespace uam::acp_detail;
	if (method == "turn/steer")
	{
		const std::unordered_map<std::string, AcpPendingSteerState>::const_iterator pending = session.pending_steer_requests.find(request_id);
		if (pending != session.pending_steer_requests.end())
		{
			const bool confirmed = uam::nlohmann_json::TrimmedStringValue(result, {"turnId"}) == pending->second.provider_turn_id;
			FinishCodexSteer(app, session, chat, request_id, confirmed ? "" : "Codex did not confirm the steered message for the requested turn. Retry the message after this turn finishes.");
		}
		return true;
	}
	if (method == uam::acp_methods::kTurnInterrupt)
	{
		session.cancel_request_id = 0;
		return true;
	}
	if (uam::acp_methods::IsCodexThreadSetupMethod(method))
	{
		session.session_setup_request_id = 0;
		if (method == uam::acp_methods::kThreadStart)
		{
			session.provider_usage.token_usage = AcpTokenUsageState{};
		}
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
		if (!session.goal_internal_session)
		{
			const std::string previous_native_session_id = chat.native_session_id;
			SetChatNativeSessionIdIfChanged(chat, session.session_id);
			SyncResolvedNativeSessionIdForChat(app, chat, session.session_id, previous_native_session_id);
		}
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
		(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
		return true;
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
		(void)SendDeferredCodexInterruptIfReady(app, session, chat);
		return true;
	}

	if (method == uam::acp_methods::kAccountRateLimitsRead)
	{
		if (const nlohmann::json* rate_limits = uam::nlohmann_json::FindObjectField(result, "rateLimits"))
		{
			MergeCodexRateLimitSnapshot(session, *rate_limits);
		}
		return true;
	}

	if (method == uam::acp_methods::kModelList)
	{
		const auto fail_discovery = [&](const std::string& error)
		{
			AppendAcpDiagnostic(session, "response", "model_discovery_failed", method, request_id, false, 0, error);
			if (app.provider_model_catalog != nullptr)
			{
				app.provider_model_catalog->RememberRefreshFailure(session.provider_id, error,
				    DiscoveryWorkspace(app, chat), chat.execution_host_id);
			}
			session.codex_model_discovery = {};
			StopBackgroundModelDiscovery(app, session);
		};
		const nlohmann::json* data = uam::nlohmann_json::FindArrayField(result, "data");
		const nlohmann::json* cursor = uam::nlohmann_json::FindField(result, "nextCursor");
		if (data == nullptr || (cursor != nullptr && !cursor->is_null() && !cursor->is_string()))
		{
			fail_discovery("Codex returned an invalid model catalog page.");
			return true;
		}
		auto& discovery = session.codex_model_discovery;
		CodexModelParseOptions parse_options;
		parse_options.skip_hidden_field = true;
		parse_options.allow_default_non_list_visibility = true;
		for (const nlohmann::json& model : *data)
		{
			std::optional<ParsedCodexModelEntry> parsed = ParseCodexModelEntry(model, parse_options);
			if (!parsed || !discovery.model_ids.insert(parsed->model.id).second) continue;
			if (parsed->is_default) discovery.current_model_id = parsed->model.id;
			discovery.models.push_back(std::move(parsed->model));
		}
		const std::string explicit_current_model = uam::nlohmann_json::TrimmedStringValue(result, {"currentModelId", "model"});
		if (!explicit_current_model.empty()) discovery.current_model_id = explicit_current_model;
		const std::string next_cursor = cursor != nullptr && cursor->is_string() ? cursor->get<std::string>() : std::string();
		if (!next_cursor.empty())
		{
			if (!discovery.cursors.insert(next_cursor).second)
			{
				fail_discovery("Codex repeated a model catalog cursor.");
				return true;
			}
			const int next_id = NextAcpRequestId(session, uam::acp_methods::kModelList);
			std::string error;
			if (!WriteAcpMessage(session, BuildModelListRequest(next_id, next_cursor), &error))
			{
				session.pending_request_methods.erase(next_id);
				fail_discovery(error);
			}
			return true;
		}
		if (discovery.models.empty())
		{
			fail_discovery("Codex model discovery completed without reporting any models.");
			return true;
		}
		session.available_models = std::move(discovery.models);
		if (!discovery.current_model_id.empty() && !session.session_ready)
		{
			session.current_model_id = std::move(discovery.current_model_id);
		}
		discovery = {};
		RememberDiscoveredModels(app, session, chat);
		StopBackgroundModelDiscovery(app, session);
		return true;
	}

	return false;
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool can_load, std::string& out_method) const
{
	(void)can_load;
	const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	nlohmann::json params = {
	    {"cwd", cwd}, {"approvalPolicy", "on-request"}, {"sandbox", "workspace-write"}, {"persistExtendedHistory", true},
	};
	if (resume_id.empty())
	{
		out_method = uam::acp_methods::kThreadStart;
		params["serviceName"] = uam::acp_request_defaults::kClientName;
		params["experimentalRawEvents"] = false;
	}
	else
	{
		out_method = uam::acp_methods::kThreadResume;
		params["threadId"] = resume_id;
	}
	const std::string model_id = uam::strings::Trim(chat.model_id);
	if (!model_id.empty()) params["model"] = model_id;
	return uam::acp_json_rpc::Request(request_id, out_method, std::move(params));
}

std::string CodexCliProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
    const std::string& prompt, const ChatSession& chat, std::string& out_method) const
{
	out_method = uam::acp_methods::kTurnStart;
	nlohmann::json params = {
	    {"threadId", session.session_id},
	    {"input", nlohmann::json::array({{{"type", "text"}, {"text", prompt}, {"text_elements", nlohmann::json::array()}}})},
	};

	const std::string model_id = uam::strings::Trim(chat.model_id);
	const std::string collaboration_model_id = model_id.empty() ? uam::strings::Trim(session.current_model_id) : model_id;
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
	if (chat.service_tier_explicit) params["serviceTier"] = uam::nlohmann_json::StringOrNull(service_tier);

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

std::string CodexCliProviderRuntime::AcpTurnIdentity(const uam::AcpSessionState& session) const
{
	return session.codex_turn_id;
}

void CodexCliProviderRuntime::RestoreAcpTurnIdentity(uam::AcpSessionState& session, const std::string& identity) const
{
	if (session.codex_turn_id.empty()) session.codex_turn_id = identity;
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildSteer(uam::AcpSessionState& session,
    int request_id, const std::string& prompt, std::string& out_method) const
{
	if (session.session_id.empty() || session.codex_turn_id.empty()) return nullptr;
	out_method = "turn/steer";
	session.pending_steer_requests[std::to_string(request_id)].provider_turn_id = session.codex_turn_id;
	return uam::acp_json_rpc::Request(request_id, out_method, {
	    {"threadId", session.session_id},
	    {"expectedTurnId", session.codex_turn_id},
	    {"input", nlohmann::json::array({{{"type", "text"}, {"text", prompt}, {"text_elements", nlohmann::json::array()}}})},
	});
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState& session,
    int request_id, std::string& out_method) const
{
	if (!session.session_id.empty() && !session.codex_turn_id.empty())
	{
		out_method = uam::acp_methods::kTurnInterrupt;
		return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kTurnInterrupt,
		                                  {
		                                      {"threadId", session.session_id},
		                                      {"turnId", session.codex_turn_id},
		                                  });
	}
	out_method.clear();
	return nullptr;
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
    const std::string& option_id, bool cancelled) const
{
	const std::string kind = session.pending_permission.provider_request_kind;
	const bool deny = uam::acp_permissions::IsDenyDecision(option_id, cancelled);
	nlohmann::json response = uam::acp_json_rpc::SuccessResponse(
	    uam::acp_detail::StableStringToJsonRpcId(session.pending_permission.request_id_json),
	    nlohmann::json::object());

	if (uam::acp_permissions::IsCodexDecisionPermissionKind(kind))
	{
		response["result"] = {{"decision", uam::acp_permissions::CodexDecisionForOption(option_id, cancelled)}};
	}
	else if (kind == uam::acp_permissions::kCodexPermissionsRequestKind)
	{
		nlohmann::json permissions = nlohmann::json::object();
		if (!deny && !session.pending_permission.codex_approval_payload_json.empty())
		{
			try
			{
				const nlohmann::json payload = nlohmann::json::parse(session.pending_permission.codex_approval_payload_json);
				const nlohmann::json* parsed_permissions = uam::nlohmann_json::FindField(payload, "permissions");
				if (parsed_permissions != nullptr)
				{
					permissions = *parsed_permissions;
				}
			}
			catch (const nlohmann::json::exception&)
			{
				permissions = nlohmann::json::object();
			}
		}
		response["result"] = {
		    {uam::acp_permissions::kPermissionsField, permissions},
		    {uam::acp_permissions::kScopeField, uam::acp_permissions::kSessionScope},
		};
	}
	else
	{
		response["result"] = nlohmann::json::object();
	}
	return response;
}

bool CodexCliProviderRuntime::OnAcpTryAutoApprove(uam::AcpSessionState& session, const ChatSession& chat,
    std::string* error_out) const
{
	(void)session;
	(void)chat;
	(void)error_out;
	return false;
}

std::string CodexCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	return mode_id;
}

const IProviderRuntime& GetCodexCliProviderRuntime()
{
	static const CodexCliProviderRuntime runtime;
	return runtime;
}
