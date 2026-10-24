#include "common/provider/claude/cli/claude_cli_provider_runtime.h"
#include "common/provider/claude/cli/claude_preinput_identity.h"
#include "common/config/execution_host_config.h"
#include "common/utils/uuid.h"

#include <chrono>
#include <limits>
#include <memory>

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/approval_modes.h"
#include "common/security/command_safety.h"
#include "common/provider/provider_ids.h"
#include "common/state/app_state.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/provider/claude/cli/claude_acp_message_handlers.h"
#include "common/utils/range_utils.h"
#include "common/utils/base64.h"
#include "common/paths/path_utils.h"
#include "common/platform/async_byte_writer.h"
#include "common/utils/nlohmann_json_utils.h"
#include "app/attachment_preview_service.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
	constexpr auto kClaudeProviderPermissionModes = std::to_array<std::string_view>({
	    uam::approval_modes::kDefaultApprovalMode,
	    uam::approval_modes::kPlanApprovalMode,
	});


	bool ShouldPassClaudePermissionMode(std::string_view approval_mode)
	{
		return uam::ranges::Contains(kClaudeProviderPermissionModes, approval_mode);
	}

	std::string ClaudeStructuredPermissionMode(const ChatSession& chat)
	{
		if (uam::strings::TrimAsciiView(chat.approval_mode) == uam::approval_modes::kPlanApprovalMode) return uam::approval_modes::kPlanApprovalMode;
		const uam::command_safety::Tier tier = uam::command_safety::ParseTier(chat.command_safety_tier);
		if (tier == uam::command_safety::Tier::AiReview) return uam::approval_modes::kProviderAutoApprovalMode;
		if (tier == uam::command_safety::Tier::AcceptEdits) return uam::approval_modes::kAcceptEditsApprovalMode;
		return uam::approval_modes::kDefaultApprovalMode;
	}

	void AppendClaudeModeArgs(std::vector<std::string>& argv, const ChatSession& chat, const AppSettings& settings)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
		const std::string effort = uam::strings::ToLowerAscii(uam::strings::Trim(chat.reasoning_effort));
		if (effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh" || effort == "max")
			uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", effort);

		const std::string approval_mode = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, "off");
		if (ShouldPassClaudePermissionMode(approval_mode))
		{
			uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", approval_mode);
		}
	}

	/// <summary>Preserves the effective inline settings object; remote file settings cannot be merged on the desktop.</summary>
	void AppendClaudeTerminalFastMode(std::vector<std::string>& argv, const ChatSession& chat)
	{
		if (chat.service_tier != "fast" && !chat.service_tier_explicit) return;
		nlohmann::json settings = nlohmann::json::object();
		std::optional<std::size_t> last;
		std::size_t count = 0;
		for (std::size_t index = 1; index < argv.size() && argv[index] != "--"; ++index)
		{
			if (argv[index] == "--settings" && index + 1 < argv.size())
			{
				last = index; count = 2; ++index;
			}
			else if (argv[index].starts_with("--settings=")) { last = index; count = 1; }
		}
		if (last)
		{
			const std::string value = count == 2 ? argv[*last + 1] : argv[*last].substr(std::string("--settings=").size());
			settings = nlohmann::json::parse(value, nullptr, false);
			if (!settings.is_object()) return;
		}
		settings["fastMode"] = chat.service_tier == "fast";
		if (last)
		{
			if (count == 2) argv[*last + 1] = settings.dump();
			else argv[*last] = "--settings=" + settings.dump();
		}
		else
		{
			const auto positional = std::find(argv.begin() + std::min(std::size_t{1}, argv.size()), argv.end(), "--");
			argv.insert(positional, {"--settings", settings.dump()});
		}
	}
} // namespace

const ProviderCliPolicy* ClaudeCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kClaudeCli,
		.npm_package = "@anthropic-ai/claude-code",
		.fallback_title = "Claude Code",
		.executable_name = "claude",
		.version_probe_command = "claude --version",
		.homebrew_package = "claude-code",
		.homebrew_cask = true,
		.preferred_version = "latest",
		.provider_managed = true,
	};
	return &policy;
}

const char* ClaudeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kClaudeCli;
}


std::vector<std::string> ClaudeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "claude");
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	AppendClaudeModeArgs(argv, chat, provider_settings);
	uam::provider_runtime_internal::AppendArgs(argv, uam::provider_runtime_internal::BuildProviderFlagsArgv(provider_settings));
	AppendClaudeTerminalFastMode(argv, chat);
	return argv;
}

MessageRole ClaudeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ClaudeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool ClaudeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}


std::vector<std::string> ClaudeCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"claude", "-p"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add stateless args for worker mode
	argv.push_back("--no-session-persistence");
	argv.push_back("--tools");
	argv.push_back("");

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);

	argv.push_back("--");
	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> ClaudeCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"claude", "-p", "--output-format", "stream-json", "--input-format", "stream-json", "--verbose", "--permission-prompt-tool", "stdio"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", ClaudeStructuredPermissionMode(chat));
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	const std::string effort = uam::strings::ToLowerAscii(uam::strings::Trim(chat.reasoning_effort));
	if (effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh" || effort == "max")
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", effort);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--resume", chat.native_session_id);
	uam::computer_use::AppendClaudeMcpLaunchArguments(argv, chat);
	return argv;
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	session.initialized = false;
	session.claude_manual_compaction_supported = false;
	session.claude_manual_compaction_boundary_seen = false;
	session.load_session_supported = true;
	session.available_modes = {
	    uam::AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Claude's provider-managed permissions."},
	    uam::AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Let Claude research and propose changes without editing files."},
	};
	if (session.current_mode_id.empty())
	{
		session.current_mode_id = uam::approval_modes::kDefaultApprovalMode;
	}
	return uam::acp_claude_stream::ControlRequest(request_id, {{"subtype", "initialize"}});
}

bool ClaudeCliProviderRuntime::OnAcpPreDispatch(uam::AppState& app, uam::AcpSessionState& session, const ChatSession& chat,
    const nlohmann::json* decoded_frame) const noexcept
{
	return uam::claude_preinput::BeforeSharedDispatch(app, session, chat, decoded_frame);
}

bool ClaudeCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const
{
	using namespace uam::acp_detail;
	try
	{
		HandleClaudeMessage(app, session, chat, message, browser);
		MarkAcpRuntimeActivity(session);
	}
	catch (const std::exception& ex)
	{
		const std::string error_message = std::string("Claude stream-json message handling failed: ") + ex.what();
		AppendAcpDiagnostic(session, "parse", "claude_message_parse_error", "", "", false, 0,
		                    error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		InvalidateAcpTransport(app, session, chat, error_message);
	}
	return true;
}

void ClaudeCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	session.available_models.clear();
	for (const nlohmann::json& model : uam::acp_detail::JsonArrayValue(result, "models"))
	{
		if (!model.is_object())
			continue;
		const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"value"});
		if (id.empty())
			continue;
		uam::AcpModelState parsed{id, uam::acp_detail::JsonDiagnosticStringValueOr(model, "displayName", id), uam::acp_detail::JsonDiagnosticStringValue(model, "description")};
		parsed.context_window_tokens = uam::acp_models::ModelTokenLimit(model, {"contextWindowTokens", "contextWindow"});
		parsed.max_output_tokens = uam::acp_models::ModelTokenLimit(model, {"maxOutputTokens"});
		if (uam::acp_detail::JsonBooleanValueOr(model, "supportsFastMode", false)) parsed.additional_speed_tiers = {"fast"};
		if (!model.contains("supportsEffort") || uam::acp_detail::JsonBooleanValueOr(model, "supportsEffort", false))
		{
			for (const auto& level : uam::acp_detail::JsonArrayValue(model, "supportedEffortLevels"))
			{
				if (!level.is_string()) continue;
				const std::string effort = level.get<std::string>();
				if ((effort == "low" || effort == "medium" || effort == "high" || effort == "xhigh" || effort == "max") &&
				    !uam::ranges::Contains(parsed.supported_reasoning_efforts, effort)) parsed.supported_reasoning_efforts.push_back(effort);
			}
		}
		session.available_models.push_back(std::move(parsed));
	}
	session.claude_manual_compaction_supported = false;
	for (const auto& command : uam::acp_detail::JsonArrayValue(result, "commands"))
		if (command.is_object() && uam::acp_detail::JsonDiagnosticStringValue(command, "name") == "compact" &&
		    (!command.contains("builtin") || uam::acp_detail::JsonBooleanValueOr(command, "builtin", false)))
			session.claude_manual_compaction_supported = true;
	session.agent_name = "claude";
	session.agent_title = "Claude Code";
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool can_load, std::string& out_method) const
{
	(void)cwd;
	(void)can_load;
	out_method.clear();
	if (chat.service_tier == "fast" || chat.service_tier_explicit)
	{
		out_method = "claude/apply_flag_settings";
		return uam::acp_claude_stream::ControlRequest(request_id,
		    {{"subtype", "apply_flag_settings"}, {"settings", {{"fastMode", chat.service_tier == "fast"}}}});
	}
	return nullptr;
}

bool ClaudeCliProviderRuntime::OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	const std::string& method, const std::string& request_id, const nlohmann::json&) const
{
	if (method != "claude/apply_flag_settings") return false;
	if (request_id != std::to_string(session.session_setup_request_id)) return true;
	session.session_setup_request_id = 0;
	if (chat.service_tier == "fast")
	{
		const std::string selected = chat.model_id.empty() ? "default" : chat.model_id;
		const auto advertised = std::ranges::find_if(session.available_models, [&](const uam::AcpModelState& model)
		{
			return model.id == selected && uam::ranges::Contains(model.additional_speed_tiers, std::string("fast"));
		});
		if (advertised == session.available_models.end())
		{
			session.initialized = false;
			session.session_ready = false;
			session.last_error = "The selected Claude model did not advertise fast mode. Choose Default speed or a supported model.";
			session.lifecycle_state = uam::acp_detail::kAcpLifecycleError;
			return true;
		}
	}
	session.session_id = chat.native_session_id;
	session.session_ready = true;
	session.current_mode_id = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
	session.lifecycle_state = session.processing ? uam::acp_detail::kAcpLifecycleProcessing : uam::acp_detail::kAcpLifecycleReady;
	(void)uam::acp_detail::ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
	return true;
}

bool ClaudeCliProviderRuntime::SupportsSavedImageRetry(const uam::AcpSessionState& session) const
{
	return session.protocol_kind == "claude-code-stream-json";
}

bool ClaudeCliProviderRuntime::SupportsRemoteOwnedImageRetry(const uam::AcpSessionState& session) const
{
	return SupportsSavedImageRetry(session);
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id, const std::string& prompt, const ChatSession& chat, std::string& out_method) const
{
	(void)request_id;
	out_method.clear();
	// Remote paths belong to the runner; never open them on the desktop.
	const int first = session.turn_first_user_message_index >= 0 ? session.turn_first_user_message_index : session.turn_user_message_index;
	const std::vector<MessageAttachment> images = chat.execution_host_id == "local"
	    ? uam::claude_input::TurnImages(chat, first, session.turn_user_message_index) : std::vector<MessageAttachment>{};
	return uam::claude_input::Build(prompt, uam::paths::PathFromUtf8(chat.workspace_directory), images, session.last_error, false);
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState&, int request_id, std::string& out_method) const
{
	out_method = "claude/interrupt";
	return uam::acp_claude_stream::ControlRequest(request_id, {{"subtype", "interrupt"}});
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildCompact(const uam::AcpSessionState& session, int, std::string& out_method) const
{
	out_method.clear();
	if (!session.claude_manual_compaction_supported || session.session_id.empty()) return nullptr;
	out_method = "claude/compact";
	return {{"type", "user"}, {"session_id", session.session_id}, {"parent_tool_use_id", nullptr},
	    {"message", {{"role", "user"}, {"content", "/compact"}}}};
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session, const uam::AcpPendingPermissionState& pending, const std::string& option_id, bool cancelled) const
{
	(void)session;
	const bool deny = cancelled || option_id != "allow";
	nlohmann::json input = nlohmann::json::parse(pending.provider_input_json, nullptr, false);
	nlohmann::json result = {{"behavior", "deny"}, {"message", "Permission denied by the user."}};
	if (!deny && input.is_object())
		result = {{"behavior", "allow"}, {"updatedInput", std::move(input)}};
	return uam::acp_claude_stream::ControlResponse(uam::acp_detail::StableStringToJsonRpcId(pending.request_id_json), std::move(result));
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildUserInputResponse(const uam::AcpSessionState& session, const std::map<std::string, std::vector<std::string>>& answers) const
{
	nlohmann::json input = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
	nlohmann::json result = {{"behavior", "deny"}, {"message", "Question cancelled by the user."}};
	if (!answers.empty() && input.is_object())
	{
		nlohmann::json mapped = nlohmann::json::object();
		for (const uam::AcpUserInputQuestionState& question : session.pending_user_input.questions)
		{
			const auto found = answers.find(question.id);
			if (found != answers.end())
				mapped[question.question] = uam::strings::Join(found->second, ", ");
		}
		input["answers"] = std::move(mapped);
		result = {{"behavior", "allow"}, {"updatedInput", std::move(input)}};
	}
	return uam::acp_claude_stream::ControlResponse(uam::acp_detail::StableStringToJsonRpcId(session.pending_user_input.request_id_json), std::move(result));
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}


namespace uam::claude_preinput
{
	class Observation
	{
	  public:
		Observation(const AppState& app, AcpSessionState& runtime, const ChatSession& chat, std::shared_ptr<const InjectedTransportForTests> transport = {})
		    : injected(std::move(transport)), owner(&runtime), root(app.data_root), incarnation(chat.metadata_incarnation), epoch(runtime.permission_review_incarnation),
		      writer(runtime.stdin_writer), chat_id(chat.id), birth(chat.created_at), native(chat.native_session_id), model(chat.model_id),
		      mode(ClaudeStructuredPermissionMode(chat)), workspace(chat.workspace_directory), process(injected ? injected->process() : acp_detail::AcpProcessHandleLabel(runtime)), token(runtime.remote_output_delivery_token),
		      runner_protocol(runtime.remote_runner_protocol_version), deadline(std::chrono::steady_clock::now() + std::chrono::seconds(25))
		{
			if (injected) injected_writer = injected->writer();
			if (const auto* selected = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id)) host = *selected;
			else { Refuse(runtime); return; }
			initial = outstanding = runtime.initialize_request_id; method = "initialize";
			if (!Matches(app, runtime, chat) || initial <= 0 || runtime.pending_request_methods.size() != 1 ||
			    !runtime.pending_request_methods.contains(initial) || runtime.pending_request_methods.at(initial) != method ||
			    runtime.next_request_id <= initial || runtime.next_request_id > std::numeric_limits<int>::max() - 2) Refuse(runtime);
		}

		bool OriginalTransport(const AcpSessionState& runtime) const noexcept
		{
			try
			{
				return owner == &runtime && !epoch.expired() && epoch.lock() == runtime.permission_review_incarnation &&
				       (injected ? (!injected_writer.expired() && injected_writer.lock() == injected->writer()) : (!writer.expired() && writer.lock() == runtime.stdin_writer)) &&
				       process != "0" && !process.empty() && process == (injected ? injected->process() : acp_detail::AcpProcessHandleLabel(runtime)) &&
				       runtime.process_execution_host_id == host.id && token == runtime.remote_output_delivery_token && runner_protocol == runtime.remote_runner_protocol_version;
			}
			catch (...) { return false; }
		}

		bool CurrentTransport(const AcpSessionState& runtime) const
		{
			return runtime.provider_id == "claude-cli" && runtime.protocol_kind == "claude-code-stream-json" &&
			       runtime.running && runtime.transport_ownership_error.empty() && runtime.lifecycle_state != acp_detail::kAcpLifecycleError && !runtime.process_exit_observed && !runtime.remote_source_exit_reported && !runtime.cancel_requested &&
			       !runtime.local_stop_pending && !runtime.remote_stop_pending && !runtime.remote_stop_unconfirmed && OriginalTransport(runtime);
		}

		bool OwnedBy(const AcpSessionState& runtime) const
		{
			return owner == &runtime;
		}
		bool Injected() const { return static_cast<bool>(injected); }

		bool Current(const AppState& app, const AcpSessionState& runtime, const ChatSession& chat) const
		{
			return runtime.native_pre_dispatch_runtime == &GetClaudeCliProviderRuntime() && Matches(app, runtime, chat);
		}

		bool Matches(const AppState& app, const AcpSessionState& runtime, const ChatSession& chat) const
		{
			const auto* selected = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			const bool remote = host.id != execution_hosts::kLocalHostId;
			const bool startup = state != State::HandedOff;
			return state != State::Refused && state != State::InputAttempted && std::chrono::steady_clock::now() < deadline && CurrentTransport(runtime) &&
			       !root.empty() && root == app.data_root && !incarnation.expired() && incarnation.lock() == chat.metadata_incarnation && chat_id == chat.id && birth == chat.created_at &&
			       Bounded(chat_id, 128) && Bounded(birth, 64) && workspace == chat.workspace_directory && chat.provider_id == "claude-cli" && selected && execution_hosts::SameConnection(host, *selected) && host.id == chat.execution_host_id &&
			       uuid::IsCanonicalUuid(native) && native == chat.native_session_id && runtime.session_id == native &&
			       Bounded(model, 160) && model != "default" && model == chat.model_id && mode == ClaudeStructuredPermissionMode(chat) &&
			       chat.reasoning_effort.empty() && !chat.service_tier_explicit && (chat.service_tier.empty() || chat.service_tier == "default") &&
			       runtime.session_setup_request_id == 0 &&
			       (startup ? (!runtime.initialized && !runtime.session_ready && !runtime.processing && runtime.prompt_request_id == 0 && runtime.queued_prompt.empty() && !runtime.pending_prompt_delivery && runtime.queued_user_prompts.empty() && chat.acp_queued_prompts.empty()) :
			           (runtime.initialized && runtime.session_ready && runtime.current_model_id == model && runtime.current_mode_id == mode)) &&
			       !runtime.pending_local_launch && !runtime.pending_agent_input && !runtime.goal_internal_session && !runtime.model_discovery_only &&
			       (!remote || (chat.remote_process_exists && !token.empty() && token.size() <= 256 && token == chat.remote_process_control_token &&
			                    runner_protocol >= 3 && runner_protocol == host.runner_protocol_version && chat.remote_structured_protocol == runtime.protocol_kind));
		}

		void Refuse(AcpSessionState& runtime)
		{
			if (!OwnedBy(runtime) || state == State::Refused) return;
			state = State::Refused; tuple.reset();
			if (!OriginalTransport(runtime)) return; // Retire only the original generation, once. Never revive cleanup from restored metadata.
			const auto found = runtime.pending_request_methods.find(outstanding);
			if (outstanding > 0 && found != runtime.pending_request_methods.end() && found->second == method) runtime.pending_request_methods.erase(found);
			if (runtime.initialize_request_id == initial) runtime.initialize_request_id = 0;
			// Keep this observation installed, even after failure. No ordinary-handler
			// fallback, native ID/model projection, save, user input or auto-restage.
		}

		void Consume(const AppState& app, AcpSessionState& runtime, const ChatSession& chat, const nlohmann::json& message)
		{
			try
			{
				if (!Current(app, runtime, chat) || !message.is_object() || state == State::Observed ||
				    !Equals(message, "type", "control_response") || !RootScope(message)) { Refuse(runtime); return; }
				const auto response = message.find("response");
				if (response == message.end() || !response->is_object() || !Equals(*response, "subtype", "success") ||
				    !Equals(*response, "request_id", std::to_string(outstanding)) || runtime.pending_request_methods.size() != 1 ||
				    !runtime.pending_request_methods.contains(outstanding) || runtime.pending_request_methods.at(outstanding) != method)
				{ Refuse(runtime); return; }
				const auto payload = response->find("response");
				if (payload == response->end() || !payload->is_object()) { Refuse(runtime); return; }
				if (state == State::Initialize)
				{
					if (!Equals(*payload, "claude_code_version", "2.1.302") || !Equals(*payload, "current_permission_mode", mode) ||
					    !EmptyArray(*response, "pending_permission_requests") || !EmptyArray(*response, "pending_user_dialog_requests"))
					{ Refuse(runtime); return; }
					native_mode = payload->at("current_permission_mode").get<std::string>(); native_version = payload->at("claude_code_version").get<std::string>();
					Retire(runtime); runtime.initialize_request_id = 0;
					Send(runtime, State::Status, "claude/preinput_get_status", "get_status");
					return;
				}
				if (state == State::Status)
				{
					const auto sections = payload->find("sections");
					if (sections == payload->end() || !sections->is_array() || sections->empty() || sections->size() > 8) { Refuse(runtime); return; }
					std::string observed; std::size_t count = 0;
					for (const auto& section : *sections)
					{
						if (!section.is_object()) { Refuse(runtime); return; }
						if (!Equals(section, "title", "Session")) continue;
						const auto rows = section.find("rows");
						if (rows == section.end() || !rows->is_array() || rows->size() > 32) { Refuse(runtime); return; }
						for (const auto& row : *rows)
						{
							if (!row.is_object()) { Refuse(runtime); return; }
							if (!Equals(row, "label", "Session ID")) continue;
							if (++count != 1 || !Equals(row, "value", native)) { Refuse(runtime); return; }
							observed = row.at("value").get<std::string>();
						}
					}
					if (count != 1) { Refuse(runtime); return; }
					native_id = std::move(observed); Retire(runtime);
					Send(runtime, State::Settings, "claude/preinput_get_settings", "get_settings");
					return;
				}
				if (state == State::Settings)
				{
					const auto applied = payload->find("applied");
					if (applied == payload->end() || !applied->is_object() || !Equals(*applied, "model", model)) { Refuse(runtime); return; }
					const std::string native_model = applied->at("model").get<std::string>();
					Retire(runtime); tuple = NativeTuple(native_id, native_model, native_mode, native_version); state = State::Observed;
					return;
				}
				Refuse(runtime);
			}
			catch (...) { Refuse(runtime); } // Never log account/settings payloads or decoder exception text.
		}

		State state = State::Initialize;
		std::optional<NativeTuple> tuple;

	  private:
		static bool Bounded(const std::string& value, std::size_t limit)
		{
			return !value.empty() && value.size() <= limit && strings::TrimAsciiView(value) == value &&
			       std::ranges::none_of(value, [](unsigned char c) { return c < 32 || c == 127; });
		}
		static bool Equals(const nlohmann::json& object, const char* key, const std::string& expected)
		{
			const auto field = object.find(key);
			return field != object.end() && field->is_string() && field->get_ref<const std::string&>() == expected;
		}
		static bool EmptyArray(const nlohmann::json& object, const char* key)
		{
			const auto field = object.find(key); return field != object.end() && field->is_array() && field->empty();
		}
		bool RootScope(const nlohmann::json& message) const
		{
			const auto parent = message.find("parent_tool_use_id");
			if (parent != message.end() && !parent->is_null() && (!parent->is_string() || !parent->get_ref<const std::string&>().empty())) return false;
			const auto scope = message.find("session_id");
			return scope == message.end() || (scope->is_string() && scope->get_ref<const std::string&>() == native);
		}
		void Retire(AcpSessionState& runtime)
		{
			runtime.pending_request_methods.erase(outstanding); outstanding = 0; method.clear();
		}
		void Send(AcpSessionState& runtime, State next, const char* registered_method, const char* subtype)
		{
			if (runtime.next_request_id <= 0 || runtime.next_request_id == std::numeric_limits<int>::max()) { Refuse(runtime); return; }
			outstanding = runtime.next_request_id++; method = registered_method; state = next;
			runtime.pending_request_methods.emplace(outstanding, method);
			const auto request = acp_claude_stream::ControlRequest(outstanding, {{"subtype", subtype}});
			if (!(injected ? injected->write_control(request) : acp_detail::WriteAcpMessage(runtime, request))) Refuse(runtime);
		}
		const std::shared_ptr<const InjectedTransportForTests> injected;
		std::weak_ptr<const void> injected_writer;
		const AcpSessionState* const owner;
		const std::filesystem::path root;
		const std::weak_ptr<const std::uint8_t> incarnation;
		const std::weak_ptr<const char> epoch;
		const std::weak_ptr<platform::AsyncByteWriter> writer;
		const std::string chat_id, birth, native, model, mode, workspace, process, token;
		const int runner_protocol;
		ExecutionHost host;
		const std::chrono::steady_clock::time_point deadline;
		int initial = 0, outstanding = 0;
		std::string method, native_id, native_mode, native_version;
	};

	bool Arm(const AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		if (session.claude_preinput_observation || session.native_pre_dispatch_runtime) return false;
		session.claude_preinput_observation = std::make_shared<Observation>(app, session, chat);
		// Static adapter lifetime, bound at this actual process observation. Subsequent
		// metadata drift must not choose a different adapter before the guard sees it.
		session.native_pre_dispatch_runtime = &GetClaudeCliProviderRuntime();
		return session.claude_preinput_observation->state == State::Initialize;
	}
	bool ArmWithInjectedTransportForTests(const AppState& app, AcpSessionState& session, const ChatSession& chat,
	    std::shared_ptr<const InjectedTransportForTests> transport)
	{
		if (!transport || !transport->process || !transport->writer || !transport->write_control ||
		    session.claude_preinput_observation || session.native_pre_dispatch_runtime || session.next_request_id <= 0 ||
		    session.next_request_id > std::numeric_limits<int>::max() - 3) return false;
		const int id = session.next_request_id++;
		const auto request = ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpBuildInitialize(session, id);
		session.pending_request_methods[id] = "initialize";
		session.initialize_request_id = id;
		if (!transport->write_control(request)) return false;
		session.claude_preinput_observation = std::make_shared<Observation>(app, session, chat, std::move(transport));
		session.native_pre_dispatch_runtime = &GetClaudeCliProviderRuntime();
		return session.claude_preinput_observation->state == State::Initialize;
	}
	State CurrentState(const AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		const auto& observation = session.claude_preinput_observation;
		if (!observation) return State::Absent;
		if (!observation->OwnedBy(session)) return State::Refused;
		if (observation->state == State::InputAttempted) return observation->OwnedBy(session) ? State::InputAttempted : State::Refused;
		if (!observation->Current(app, session, chat)) observation->Refuse(session);
		return observation->state;
	}
	std::optional<NativeTuple> CurrentTuple(const AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		if (CurrentState(app, session, chat) != State::Observed) return std::nullopt;
		return session.claude_preinput_observation->tuple;
	}
	std::shared_ptr<const NativeWitness> CurrentWitness(const AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		if (!session.claude_preinput_observation || !session.claude_preinput_observation->OwnedBy(session))
		{
			return {};
		}
		if (CurrentState(app, session, chat) != State::Observed)
		{
			return {};
		}
		const auto& observation = session.claude_preinput_observation;
		if (!observation->tuple)
		{
			return {};
		}
		return std::shared_ptr<const NativeWitness>(new NativeWitness(observation, session, *observation->tuple, observation->Injected()));
	}
	bool NativeWitness::Current(const AppState& app, AcpSessionState& session, const ChatSession& chat) const
	{
		const auto observation = m_observation.lock();
		// Do not run a different observer or retire another owner's pending control requests.
		// A retained tuple remains historical DATA even after this exact witness is refused.
		if (m_refused || &session != m_owner || !observation || session.claude_preinput_observation != observation)
		{
			m_refused = true;
			return false;
		}
		const auto state = CurrentState(app, session, chat);
		if (state != State::Observed && state != State::HandedOff)
		{
			m_refused = true;
			return false;
		}
		return true;
	}
	bool NativeWitness::Handoff(const AppState& app, AcpSessionState& session, const ChatSession& chat) const
	{
		if (!Current(app, session, chat) || session.claude_preinput_observation->state != State::Observed)
		{
			m_refused = true;
			return false;
		}
		session.current_model_id = m_tuple.Model();
		session.current_mode_id = m_tuple.Mode();
		session.initialized = true;
		session.session_ready = true;
		session.claude_preinput_observation->state = State::HandedOff;
		if (!Current(app, session, chat))
		{
			// A deadline/identity failure at the handoff cannot leave a ready projection.
			// Never rewrite a successor observer's projection while retiring this handle.
			const auto original = m_observation.lock();
			if (original && session.claude_preinput_observation == original && original->OwnedBy(session))
			{
				session.initialized = false;
				session.session_ready = false;
			}
			return false;
		}
		return true;
	}
	bool NativeWitness::InputAttempted(const AppState& app, AcpSessionState& session, const ChatSession& chat) const
	{
		if (!Current(app, session, chat) || session.claude_preinput_observation->state != State::HandedOff)
		{
			m_refused = true;
			return false;
		}
		session.claude_preinput_observation->state = State::InputAttempted;
		m_refused = true;
		return true;
	}
	bool BeforeSharedDispatch(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json* decoded_frame) noexcept
	{
		if (!session.claude_preinput_observation) return false;
		if (!session.claude_preinput_observation->OwnedBy(session)) return true; // Consume without mutation or ordinary fallback.
		if (session.claude_preinput_observation->state == State::InputAttempted) return false;
		if (decoded_frame) session.claude_preinput_observation->Consume(app, session, chat, *decoded_frame);
		else session.claude_preinput_observation->Refuse(session);
		return true;
	}
	bool Consume(const AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message)
	{
		if (!session.claude_preinput_observation) return false;
		if (!session.claude_preinput_observation->OwnedBy(session)) return true; // Consume without mutation or ordinary fallback.
		if (session.claude_preinput_observation->state == State::InputAttempted) return false;
		session.claude_preinput_observation->Consume(app, session, chat, message);
		return true;
	}
}
