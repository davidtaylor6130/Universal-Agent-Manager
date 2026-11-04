#include "common/provider/copilot/cli/copilot_managed_read_policy.h"
#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

#include "app/agent_run_scheduler.h"
#include "common/config/approval_modes.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "common/utils/uuid.h"

#include <algorithm>
#include <charconv>

namespace
{
using namespace uam;
using namespace uam::acp_detail;
using Json = nlohmann::json;

constexpr const char* kState = "managedReadPolicy";
constexpr const char* kStatus = "status.get";
constexpr const char* kSettings = "session.managedSettings.get";
constexpr const char* kPermissions = "session.permissions.getMode";
constexpr const char* kSnapshot = "session.metadata.snapshot";
constexpr const char* kInitializeTools = "session.tools.initializeAndValidate";
constexpr const char* kTools = "session.tools.getCurrentMetadata";
constexpr double kRequestSeconds = 30.0;
constexpr std::size_t kMaxReplyBytes = 256U * 1024U;

std::string Text(const Json& object, const char* key)
{
	return std::string(nlohmann_json::StringViewOrEmpty(object, key));
}

bool Flag(const Json& object, const char* key)
{
	return nlohmann_json::BoolFieldStrict(object, key).value_or(false);
}

Json* State(AcpSessionState& session)
{
	const auto found = session.copilot_native_state.find(kState);
	return found != session.copilot_native_state.end() && found->is_object() ? &*found : nullptr;
}

Json ScopeJson(const ManagedNativeReadScope& scope)
{
	return {{"run", scope.run_id}, {"turn", scope.turn_id}, {"process", scope.process_id},
	    {"rootBirth", scope.root_birth_id}, {"transcriptBirth", scope.transcript_birth_id},
	    {"model", scope.model_id}, {"workspace", scope.workspace_directory}, {"native", scope.native_session_id},
	    {"configuration", scope.configuration}, {"childPolicy", scope.child_policy},
	    {"serial", scope.turn_serial}, {"resume", scope.resume_only}};
}

ManagedNativeReadScope CapturedScope(const Json& state)
{
	const auto& value = state.at("scope");
	ManagedNativeReadScope scope;
	scope.run_id = Text(value, "run"); scope.turn_id = Text(value, "turn"); scope.process_id = Text(value, "process");
	scope.root_birth_id = Text(value, "rootBirth"); scope.transcript_birth_id = Text(value, "transcriptBirth");
	scope.model_id = Text(value, "model"); scope.workspace_directory = Text(value, "workspace"); scope.native_session_id = Text(value, "native");
	scope.configuration = Text(value, "configuration"); scope.child_policy = Text(value, "childPolicy");
	scope.turn_serial = value.at("serial").get<std::int64_t>(); scope.resume_only = Flag(value, "resume");
	return scope;
}

void RetireRequest(AcpSessionState& session, const std::string& key, const Json& request)
{
	int id = 0;
	const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
	if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size()) return;
	const auto pending = session.pending_request_methods.find(id);
	if (pending != session.pending_request_methods.end() && pending->second == Text(request, "method")) session.pending_request_methods.erase(pending);
}

void Reject(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state, std::string_view reason)
{
	if (Text(state, "phase") == "failed") return;
	state["phase"] = "failed";
	state["error"] = std::string(uam::strings::Utf8Prefix(reason, 512));
	for (auto it = state["requests"].begin(); it != state["requests"].end(); ++it) RetireRequest(session, it.key(), *it);
	state["requests"] = Json::object();
	AgentRunScheduler::RejectManagedReadPolicy(app, session, chat, CapturedScope(state), Text(state, "error"));
}

bool Owns(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state)
{
	// Retention permits normal settlement/save/reap while continuing to compare
	// the full captured policy. It cannot grant or refresh observation evidence.
	if (Text(state, "phase") == "verified")
	{
		if (AgentRunScheduler::ManagedReadPolicyRetained(app, session, chat, CapturedScope(state))) return true;
		Reject(app, session, chat, state, "The completed Copilot policy observation no longer retains its exact managed owner or configuration.");
		return false;
	}
	const auto current = AgentRunScheduler::ManagedReadOnlyNativeScope(app, session, chat);
	if (!current || ScopeJson(*current) != state.at("scope"))
	{
		Reject(app, session, chat, state, "The managed Copilot policy observation lost its exact process, turn, configuration or owner.");
		return false;
	}
	return true;
}

bool Failed(const Json& state) { return Text(state, "phase") == "failed"; }

bool Send(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state, const std::string& method)
{
	if (state["history"].size() >= 16 || state["requests"].size() >= 5)
	{
		Reject(app, session, chat, state, "Copilot exceeded the bounded managed policy observation requests.");
		return false;
	}
	const int id = NextAcpRequestId(session, method);
	const std::string key = std::to_string(id);
	const Json request = {{"method", method}, {"phase", Text(state, "phase")}, {"native", Text(state, "native")}, {"deadline", GetAppTimeSeconds() + kRequestSeconds}};
	state["history"][key] = request;
	state["requests"][key] = request;
	const Json params = method == kStatus ? Json::object() : Json{{"sessionId", Text(state, "native")}};
	if (!WriteAcpMessage(session, acp_json_rpc::Request(id, method, params)))
	{
		Reject(app, session, chat, state, "The managed Copilot native policy request could not be written.");
		return false;
	}
	return true;
}

bool Expire(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state)
{
	for (const auto& request : state["requests"])
		if (request.at("deadline").get<double>() <= GetAppTimeSeconds())
		{
			Reject(app, session, chat, state, "Copilot did not return complete managed policy evidence before the bounded observation deadline.");
			return true;
		}
	return false;
}

void BeginObservations(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state, const char* phase)
{
	state["phase"] = phase;
	state["observations"] = Json::object();
	for (const char* method : {kSettings, kPermissions, kSnapshot, kInitializeTools})
		if (!Send(app, session, chat, state, method)) break;
}

bool Complete(const Json& state)
{
	const auto& observed = state.at("observations");
	for (const char* method : {kSettings, kPermissions, kSnapshot, kInitializeTools, kTools})
		if (!Flag(observed, method)) return false;
	return true;
}

bool SettingsValid(const Json& result)
{
	// Resolved settings and permissions are JsonValue, deliberately opaque. The
	// request provides the rules; documented native provenance confirms adoption.
	const auto context = result.find("permissionsContext");
	const auto keys = result.find("managedKeys");
	const auto source = Text(result, "source");
	const bool context_valid = context == result.end() || (context->is_object() && nlohmann_json::BoolFieldStrict(*context, "failClosed") == std::optional<bool>(false));
	return Flag(result, "clientManaged") && Flag(result, "bypassPermissionsDisabled") &&
	    nlohmann_json::BoolFieldStrict(result, "failClosed") == std::optional<bool>(false) &&
	    context_valid && (source == "client" || source == "mixed") &&
	    keys != result.end() && keys->is_array() && keys->size() <= 128 &&
	    std::ranges::any_of(*keys, [](const Json& key) { return key == "permissions"; });
}

bool ToolsValid(const Json& result)
{
	const auto tools = result.find("tools");
	if (tools == result.end() || !tools->is_array() || tools->size() > 3) return false;
	std::vector<std::string> names;
	for (const auto& tool : *tools)
	{
		const auto name = Text(tool, "name");
		if ((name != "view" && name != "grep" && name != "glob") || std::ranges::find(names, name) != names.end() || !Text(tool, "mcpServerName").empty() || !Text(tool, "mcpToolName").empty()) return false;
		const auto qualified = Text(tool, "namespacedName");
		// This is a conservative catalogue compatibility check, not an origin
		// attestation. The source-qualified setup filters enforce tool dispatch.
		if (!qualified.empty() && qualified != "builtin:" + name) return false;
		names.push_back(name);
	}
	return true; // An empty/subset catalogue can represent additional native restrictions.
}

bool SupportedVersion(std::string_view version)
{
	if (version.empty() || version.size() > 80) return false;
	int dots = 0;
	bool digit = false;
	for (const char character : version)
	{
		if (character >= '0' && character <= '9') digit = true;
		else if (character == '.' && digit && dots < 2) { ++dots; digit = false; }
		else return false;
	}
	return dots == 2 && digit && CliProviderVersionAtLeast(version, "1.0.98");
}

bool SnapshotValid(const Json& result, const Json& state)
{
	const auto scope = CapturedScope(state);
	const auto mode = Text(result, "currentMode");
	return Text(result, "sessionId") == Text(state, "native") && Text(result, "selectedModel") == scope.model_id &&
	    Text(result, "workingDirectory") == scope.workspace_directory &&
	    ((Text(state, "phase") == "before" && !scope.resume_only) ? (mode == "interactive" || mode == "plan") : mode == "plan");
}

bool Publish(AppState& app, AcpSessionState& session, ChatSession& chat, Json& state)
{
	const auto scope = CapturedScope(state);
	if (!AgentRunScheduler::ManagedReadPolicyObserved(app, session, chat, scope, Text(state, "native"), scope.model_id, scope.workspace_directory, "plan"))
	{
		Reject(app, session, chat, state, "Copilot native policy evidence did not retain the current managed input and save owner.");
		return false;
	}
	state["phase"] = "verified";
	if (!scope.resume_only) ScheduleChatSave(app, chat, 0.0);
	return true;
}

bool Drive(AppState& app, AcpSessionState& session, const ChatSession& chat, Json& state)
{
	if (!Owns(app, session, chat, state) || Failed(state)) return false;
	if (Expire(app, session, chat, state)) return true;
	if (Text(state, "phase") == "setup" && Flag(state, "setupConfirmed") && session.session_ready)
	{
		if (Text(state, "native").empty() || session.session_id != Text(state, "native") || chat.native_session_id != Text(state, "native"))
			Reject(app, session, chat, state, "Copilot did not retain the exact native identity established by managed setup.");
		else BeginObservations(app, session, chat, state, "before");
		return true;
	}
	if (Text(state, "phase") == "before" && Flag(state, "beforeReady") && !CapturedScope(state).resume_only && Flag(session.copilot_native_state, "sendAccepted"))
	{
		BeginObservations(app, session, chat, state, "after");
		return true;
	}
	return false;
}

AcpRequestPreflight PreflightResult(const Json& state, std::string& error)
{
	if (Failed(state))
	{
		if (!CapturedScope(state).resume_only) return AcpRequestPreflight::Ready;
		error = Text(state, "error");
		return AcpRequestPreflight::Rejected;
	}
	return Flag(state, "beforeReady") ? AcpRequestPreflight::Ready : AcpRequestPreflight::Pending;
}
}

AcpRequestPreflight CopilotCliProviderRuntime::OnAcpPrepareSetup(uam::AppState& app, uam::AcpSessionState& session,
    const ChatSession& chat, const std::string& cwd, std::string& error) const
{
	auto* state = State(session);
	const auto scope = uam::AgentRunScheduler::ManagedReadOnlyNativeScope(app, session, chat);
	if (!state)
	{
		if (!scope) return AcpRequestPreflight::Ready;
		session.copilot_native_state[kState] = {{"scope", ScopeJson(*scope)}, {"phase", "version"}, {"native", ""},
		    {"requests", Json::object()}, {"history", Json::object()}, {"observations", Json::object()}, {"beforeReady", false}};
		state = State(session);
		if (cwd != scope->workspace_directory) Reject(app, session, chat, *state, "The managed Copilot setup workspace changed before native policy negotiation.");
		else (void)Send(app, session, chat, *state, kStatus);
	}
	if (!Owns(app, session, chat, *state)) { error = Text(*state, "error"); return AcpRequestPreflight::Rejected; }
	if (Failed(*state)) return PreflightResult(*state, error);
	(void)Expire(app, session, chat, *state);
	if (Failed(*state)) return PreflightResult(*state, error);
	return Flag(*state, "supported") ? AcpRequestPreflight::Ready : AcpRequestPreflight::Pending;
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildSessionSetupRequest(uam::AcpSessionState& session, int request_id,
    const ChatSession& chat, const std::string& cwd, bool can_load, std::string& out_method) const
{
	auto request = OnAcpBuildSetupRequest(request_id, chat, cwd, can_load, out_method);
	auto* state = State(session);
	if (!state || Failed(*state) || !Flag(*state, "supported")) return request;
	const auto scope = CapturedScope(*state);
	const bool exact = scope.process_id == session.last_process_id && scope.turn_serial == session.turn_serial && scope.model_id == chat.model_id &&
	    scope.workspace_directory == cwd && (scope.resume_only ? (out_method == "session.resume" && scope.native_session_id == chat.native_session_id) : (out_method == "session.create" && chat.native_session_id.empty()));
	if (!exact)
	{
		session.last_error = "The managed Copilot setup packet lost its captured process, turn, model, workspace or exact saved native identity.";
		return nullptr; // Nonempty method distinguishes a rejected packet from skip-wire setup.
	}
	auto& params = request["params"];
	params["availableTools"] = Json::array({"builtin:view", "builtin:grep", "builtin:glob"});
	params["excludedTools"] = Json::array({"mcp:*", "custom:*"});
	params["toolFilterPrecedence"] = "excluded";
	params["managedSettings"] = {{"permissions", {{"disableBypassPermissionsMode", "disable"}, {"deny", Json::array({"shell", "write"})}}}};
	(*state)["setupId"] = std::to_string(request_id);
	(*state)["setupMethod"] = out_method;
	(*state)["phase"] = "setup";
	return request;
}

AcpRequestPreflight CopilotCliProviderRuntime::OnAcpPrepareInput(uam::AppState& app, uam::AcpSessionState& session,
    const ChatSession& chat, bool, std::string& error) const
{
	auto* state = State(session);
	if (!state) return AcpRequestPreflight::Ready;
	(void)Drive(app, session, chat, *state);
	if (!Owns(app, session, chat, *state)) { error = Text(*state, "error"); return AcpRequestPreflight::Rejected; }
	return PreflightResult(*state, error);
}

namespace uam::copilot::managed_read
{
bool HandleResult(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& method,
    const std::string& request_id, const Json& result)
{
	auto* state = State(session);
	if (!state) return false;
	if ((method == "session.create" || method == "session.resume") && request_id == Text(*state, "setupId"))
	{
		if (!Owns(app, session, chat, *state) || Failed(*state)) return false;
		const auto native = Text(result, "sessionId");
		const auto scope = CapturedScope(*state);
		if (method != Text(*state, "setupMethod") || !uuid::IsCanonicalUuid(native) || (scope.resume_only && native != scope.native_session_id))
			Reject(app, session, chat, *state, "Copilot did not confirm the exact managed setup conversation.");
		else { (*state)["native"] = native; (*state)["setupConfirmed"] = true; }
		return false; // Ordinary native identity persistence/readiness still owns this reply.
	}
	const auto history = (*state)["history"].find(request_id);
	if (history == (*state)["history"].end() || Text(*history, "method") != method) return false;
	const auto pending = (*state)["requests"].find(request_id);
	if (pending == (*state)["requests"].end()) return true; // Retired, duplicate and late replies never repair a generation.
	const Json request = *pending;
	(*state)["requests"].erase(pending);
	if (!Owns(app, session, chat, *state) || Failed(*state)) return true;
	if (Text(request, "phase") != Text(*state, "phase") || request.at("deadline").get<double>() <= GetAppTimeSeconds() || result.dump().size() > kMaxReplyBytes)
	{
		Reject(app, session, chat, *state, "Copilot returned stale or oversized managed policy evidence.");
		return true;
	}
	if (method == kStatus)
	{
		const auto version = Text(result, "version");
		if (nlohmann_json::IntFieldStrict(result, "protocolVersion") != std::optional<int>(3) || !SupportedVersion(version))
			Reject(app, session, chat, *state, "Managed Copilot continuation requires this process to report SDK protocol 3 and stable CLI 1.0.98 or newer.");
		else { (*state)["supported"] = true; (*state)["phase"] = "setup"; }
		return true;
	}
	if (Text(request, "native") != Text(*state, "native") || session.session_id != Text(*state, "native") || chat.native_session_id != Text(*state, "native"))
	{
		Reject(app, session, chat, *state, "Copilot native policy readback belongs to a different conversation.");
		return true;
	}
	const bool valid = method == kSettings ? SettingsValid(result) : method == kPermissions ? Text(result, "mode") == "manual" :
	    method == kSnapshot ? SnapshotValid(result, *state) : method == kInitializeTools ? result.is_object() : method == kTools && ToolsValid(result);
	if (!valid)
	{
		Reject(app, session, chat, *state, "Copilot did not confirm the current managed restrictions, native tools, model, workspace or mode.");
		return true;
	}
	(*state)["observations"][method] = true;
	if (method == kInitializeTools && !Send(app, session, chat, *state, kTools)) return true;
	if (!Complete(*state)) return true;
	if (Text(*state, "phase") == "before")
	{
		(*state)["beforeReady"] = true;
		if (CapturedScope(*state).resume_only) (void)Publish(app, session, chat, *state);
	}
	else if (Text(*state, "phase") == "after") (void)Publish(app, session, chat, *state);
	return true;
}

bool HandleError(AppState& app, AcpSessionState& session, ChatSession& chat, const acp_detail::AcpResponseFailureDetails& details)
{
	auto* state = State(session);
	if (!state) return false;
	const auto history = (*state)["history"].find(details.failure.request_id);
	if (history == (*state)["history"].end() || Text(*history, "method") != details.failure.method) return false;
	if ((*state)["requests"].contains(details.failure.request_id))
		Reject(app, session, chat, *state, "Copilot does not support or could not complete the required native managed policy observations.");
	return true;
}

bool Poll(AppState& app, AcpSessionState& session, ChatSession& chat)
{
	auto* state = State(session);
	return state && Drive(app, session, chat, *state);
}

void ObserveEvent(AppState& app, AcpSessionState& session, ChatSession& chat, const Json& event)
{
	auto* state = State(session);
	if (!state || Failed(*state) || !Text(event, "agentId").empty()) return;
	const auto data = event.find("data");
	if (data == event.end() || !data->is_object() || !Text(*data, "parentToolCallId").empty()) return;
	const auto id = Text(event, "id");
	if (!id.empty())
	{
		const auto seen = session.copilot_native_state.find("events");
		if (seen != session.copilot_native_state.end() && seen->is_object() && seen->contains(id)) return;
		const auto receipt = session.session_id + ":" + id;
		if (std::ranges::any_of(chat.messages, [&](const Message& row) { return std::ranges::find(row.provider_event_ids, receipt) != row.provider_event_ids.end(); })) return;
	}
	const auto type = Text(event, "type");
	if (type == "session.mode_changed" && (CapturedScope(*state).resume_only || Flag(session.copilot_native_state, "sendAccepted") || Text(*state, "phase") == "verified") && Text(*data, "newMode") != "plan")
		Reject(app, session, chat, *state, "Copilot changed the managed read-only conversation out of native Plan mode.");
	// Startup/resume replay may precede the corresponding current readback.
	// Once that readback is accepted, fresh root drift revokes it immediately,
	// including while other required replies are pending. A later matching reply
	// cannot restore the rejected observation generation.
	const auto& observations = state->at("observations");
	if ((Flag(*state, "beforeReady") || Flag(observations, kSnapshot)) && (type == "session.model_change" || type == "session.model_deselected" || type == "session.start" || type == "session.resume" || type == "session.auto_tier_switch_failed"))
		Reject(app, session, chat, *state, "Copilot changed the managed native model or session after its current policy observation.");
	if ((Flag(*state, "beforeReady") || Flag(observations, kSettings)) && type == "session.managed_settings_resolved" && !SettingsValid(*data))
		Reject(app, session, chat, *state, "Copilot changed or withdrew the current session-local managed restrictions.");
}

void ObserveModel(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view model)
{
	auto* state = State(session);
	if (!state || Failed(*state)) return;
	// The caller has already consumed an exact owned Auto-routing request.
	if (!Owns(app, session, chat, *state)) return;
	if (model != CapturedScope(*state).model_id)
		Reject(app, session, chat, *state, "Copilot contradicted the exact named model in a current native model observation.");
}
}
