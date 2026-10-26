#include "app/opencode_native_activity_service.h"
#include "common/provider/opencode/opencode_native_identity.h"
#include "common/provider/opencode/opencode_permission_policy.h"
#include "app/chat_domain_service.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_statuses.h"
#include "remote/runner_client.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <array>
#include <charconv>
#include <limits>
#include <set>
#include <thread>

namespace uam
{

namespace
{
	using Json = nlohmann::json;
	using Clock = std::chrono::steady_clock;
	constexpr std::size_t kMaximumPermissions = 64;
	constexpr std::size_t kMaximumAncestry = 16;
	constexpr std::size_t kMaximumWorkers = 2;
	constexpr int kConsecutiveFailureLimit = 3;
	// Two bounded sets (pending permissions and Plan-entry child statuses) can each
	// need their own complete ancestry, followed by at most one reply per permission.
	constexpr std::size_t kMaximumSnapshotQueries = 2 * kMaximumPermissions * kMaximumAncestry + kMaximumPermissions + 8;
	constexpr std::string_view kRequestPrefix = "opencode-native:";

	std::string String(const Json& value, const char* key)
	{
		const auto found = value.find(key);
		return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
	}
	bool ReadPermission(const std::string& permission)
	{
		return permission == "read" || permission == "glob" || permission == "grep" || permission == "list";
	}
	bool AllowedUnderReadCeiling(const Json& permission)
	{
		return ReadPermission(String(permission, "permission")) || opencode_permissions::NativeDoomLoopQuestion(permission);
	}
	std::optional<std::string> SavedDecisionId(const AcpRemoteInteractionResponseState& saved)
	{
		const Json id = Json::parse(saved.request_id_json, nullptr, false);
		if (!id.is_string() || !id.get_ref<const std::string&>().starts_with(kRequestPrefix)) return {};
		const std::string native = id.get_ref<const std::string&>().substr(kRequestPrefix.size());
		const Json response = Json::parse(saved.response_json, nullptr, false);
		if (!opencode_native::ValidNativeId(native) || !response.is_object() || !response.contains("id") || response["id"] != id) return {};
		const auto result = response.find("result");
		if (result == response.end() || !result->is_object()) return {};
		const auto outcome = result->find("outcome");
		if (outcome == result->end() || !outcome->is_object()) return {};
		const std::string kind = String(*outcome, "outcome"), option = String(*outcome, "optionId");
		if (kind != "cancelled" && (kind != "selected" || (option != "allow-once" && option != "reject-once"))) return {};
		return native;
	}
	std::string DecisionKey(const AcpRemoteInteractionResponseState& saved)
	{
		return saved.request_id_json + '\n' + saved.response_json;
	}
	bool CanRetireDecisions(const AppState& app, const ChatSession& chat, const OpenCodeNativeActivityService::Query& query,
	    const std::string& native, const std::string& host_id, const std::string& control)
	{
		if (!query.host || chat.provider_id != "opencode-cli" || chat.native_session_id != native || chat.execution_host_id != host_id ||
		    !chat.remote_process_exists || control.empty() || chat.remote_process_control_token != control ||
		    chat.remote_opencode_native_port != query.endpoint.port || chat.remote_opencode_native_token != query.endpoint.token ||
		    paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)) != query.workspace) return false;
		const ExecutionHost* current = execution_hosts::Find(app.settings.execution_hosts, host_id);
		return current && execution_hosts::SameConnection(*query.host, *current);
	}
	bool Execute(const OpenCodeNativeActivityService::Query& query, Json& result, std::string& error, std::stop_token cancel)
	{
		if (!query.host)
			return opencode_native::Query(PlatformServicesFactory::Instance().process_service, query.endpoint, query.workspace, query.operation, result, error, cancel);
		const ExecutionHost& host = *query.host;
		remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		    host);
		client.SetProcessControlToken(query.process_id, query.control_token);
		return client.QueryOpenCodeNative(query.process_id, query.operation, result, &error, cancel);
	}

	struct Snapshot
	{
		bool ok = false;
		bool authority_conflict = false;
		std::string error;
		Json permissions = Json::array();
		Json sessions = Json::object();
		std::set<std::string> pending_ids, owned_ids, confirmed_replies;
	};
	// Separate image admission never changes the native permission observer's state.
	struct ImageOwner
	{
		std::shared_ptr<const SavedImageRetryPermission> permission;
		const OpenCodeNativeCompanion::State* companion = nullptr;
		std::string lifetime, process, native, workspace, endpoint_token, control;
		int endpoint_port = 0, turn = 0, first = -1, last = -1;
		Json settings, receipts;
		std::vector<MessageAttachment> images;
		bool operator==(const ImageOwner&) const = default;
	};
	struct ImageResult
	{
		bool ok = false;
		std::string error;
		Json facts;
	};
	struct ImageEvidence
	{
		enum class Phase { Pending, Ready, Rejected };
		ImageOwner owner;
		std::uint64_t generation = 0;
		Phase phase = Phase::Pending;
		bool in_flight = false;
		std::stop_token service_cancel;
		Clock::time_point deadline;
		ImageResult result;
	};
	void RejectImage(ImageEvidence& image, const char* error)
	{
		image.phase = ImageEvidence::Phase::Rejected;
		image.result = {};
		image.result.error = error;
	}
	bool CompatibleImageVersion(const std::string& version)
	{
		if (version.empty() || version.size() > 64) return false;
		std::array<unsigned, 3> fields{};
		std::size_t offset = 0;
		for (std::size_t index = 0; index < fields.size(); ++index)
		{
			const auto end = version.find('.', offset);
			const auto count = (end == std::string::npos ? version.size() : end) - offset;
			if (count == 0 || (count > 1 && version[offset] == '0') || (index < 2) != (end != std::string::npos)) return false;
			const auto parsed = std::from_chars(version.data() + offset, version.data() + offset + count, fields[index]);
			if (parsed.ec != std::errc{} || parsed.ptr != version.data() + offset + count) return false;
			offset += count + 1;
		}
		return fields[0] == 1 && (fields[1] > 18 || (fields[1] == 18 && fields[2] >= 35));
	}
	ImageResult ReadImageEvidence(const OpenCodeNativeActivityService::Query& base, const std::string& selected,
	    const std::string& version, const OpenCodeNativeActivityService::ExecuteQuery& execute, std::stop_token cancel)
	{
		ImageResult result;
		result.error = "OpenCode could not confirm the selected model's image support. The prompt was not sent.";
		const auto started = Clock::now();
		const auto query = [&](const char* operation, Json& value)
		{
			if (cancel.stop_requested() || Clock::now() - started > std::chrono::seconds(8)) return false;
			auto request = base; request.operation = {{"operation", operation}};
			std::string private_error;
			return execute(request, value, private_error, cancel);
		};
		// Raw configured provider records can contain credentials. They stay on this worker.
		Json health, catalog;
		if (!CompatibleImageVersion(version) || !query("health", health) || !health.is_object() ||
		    !health.contains("healthy") || health["healthy"] != true || String(health, "version") != version ||
		    !query("configProviders", catalog) || !catalog.is_object()) return result;
		const auto providers = catalog.find("providers");
		if (providers == catalog.end() || !providers->is_array() || providers->empty() || providers->size() > 1024) return result;
		std::set<std::string> ids;
		std::size_t matches = 0;
		Json facts;
		for (const auto& provider : *providers)
		{
			const std::string id = String(provider, "id");
			const auto models = provider.find("models");
			if (!provider.is_object() || id.empty() || id.size() > 512 || !ids.insert(id).second ||
			    models == provider.end() || !models->is_object() || models->size() > 16384) return result;
			std::set<std::string> model_ids;
			for (const auto& [key, model] : models->items())
			{
				const std::string model_id = String(model, "id");
				if (!model.is_object() || key.empty() || key.size() > 1024 || model_id != key ||
				    String(model, "providerID") != id || !model_ids.insert(model_id).second) return result;
				// Compare the complete native formatted choice; model IDs may contain slashes.
				if (id + "/" + key != selected) continue;
				++matches;
				const auto capabilities = model.find("capabilities");
				if (capabilities == model.end() || !capabilities->is_object()) return result;
				const auto input = capabilities->find("input");
				if (input == capabilities->end() || !input->is_object() || !input->contains("image") || (*input)["image"] != true) return result;
				facts = {{"version", version}, {"provider", id}, {"model", key}, {"selectedModel", selected}, {"image", true}};
			}
		}
		if (matches != 1 || cancel.stop_requested() || Clock::now() - started > std::chrono::seconds(8)) return result;
		result.ok = true; result.error.clear(); result.facts = std::move(facts);
		return result;
	}

	struct Identity
	{
		std::string chat, native, process, workspace, host, control;
		std::shared_ptr<const std::uint8_t> incarnation;
		bool operator==(const Identity&) const = default;
	};
}

struct OpenCodeNativeCompanion::State
{
	Identity identity;
	OpenCodeNativeActivityService::Query query;
	std::string lifetime;
	std::uint64_t image_generation = 0;
	std::optional<ImageEvidence> image;
	bool captured = false, ready = false, failed = false, in_flight = false, read_only = false, retry_recovered = false;
	int failures = 0;
	std::string error;
	Clock::time_point next{};
	std::map<std::string, Json> permissions;
	std::set<std::string> published;
	std::map<std::string, std::string> replies;
	std::set<std::string> unowned_decisions;
};

struct OpenCodeNativeActivityService::State
{
	struct Job
	{
		std::shared_ptr<OpenCodeNativeCompanion::State> owner;
		Identity identity;
		std::future<Snapshot> future;
		std::stop_source stop;
		std::map<std::string, std::string> replies;
		bool read_only = false;
	};
	struct ImageJob
	{
		std::shared_ptr<OpenCodeNativeCompanion::State> owner;
		ImageOwner identity;
		std::uint64_t generation = 0;
		std::future<ImageResult> future;
		std::stop_source stop;
	};
	std::vector<ImageJob> image_jobs;
	std::stop_source image_stop;
	ExecuteQuery execute;
	bool default_execution = false;
	std::vector<Job> jobs;
	bool stopping = false;
};

OpenCodeNativeActivityService::OpenCodeNativeActivityService(ExecuteQuery execute) : m_state(std::make_unique<State>())
{
	m_state->default_execution = !execute;
	m_state->execute = execute ? std::move(execute) : Execute;
}
OpenCodeNativeActivityService::~OpenCodeNativeActivityService() { Stop(); }

std::shared_ptr<OpenCodeNativeCompanion> OpenCodeNativeActivityService::CreateCompanion(opencode_native::Endpoint endpoint)
{
	if (!opencode_native::ValidEndpoint(endpoint)) return {};
	auto companion = std::make_shared<OpenCodeNativeCompanion>();
	companion->endpoint = std::move(endpoint);
	companion->state = std::make_shared<OpenCodeNativeCompanion::State>();
	companion->state->lifetime = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	return companion;
}

namespace
{
	std::optional<ImageOwner> CaptureImageOwner(const AcpSessionState& session, const ChatSession& settings,
	    const std::vector<MessageAttachment>& images)
	{
		const auto& permission = session.saved_image_retry;
		const auto& companion = session.opencode_native_companion;
		if (!permission || !companion || !companion->state || images.empty() || images.size() > 32 ||
		    session.provider_id != "opencode-cli" || session.protocol_kind != "opencode-acp" ||
		    !session.running || !session.initialized || !session.session_ready || !session.processing ||
		    session.model_discovery_only || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending || session.remote_source_exit_reported ||
		    session.process_execution_host_id != "local" || (!settings.execution_host_id.empty() && settings.execution_host_id != "local") ||
		    session.active_uam_agent_execution_capability == "opencode-native-agent-config" || !session.prompt_image_supported ||
		    session.saved_image_retry_turn_serial != session.turn_serial || session.turn_first_user_message_index != permission->row_index || session.turn_user_message_index != permission->row_index ||
		    session.chat_id != permission->chat_id || session.session_id.empty() || session.last_process_id.empty() || settings.native_session_id != session.session_id ||
		    session.initialize_request_id || session.session_setup_request_id || session.startup_model_request_id || session.reasoning_change_request_id || session.config_option_change_request_id || session.mode_change_request_id || session.model_change_request_id || session.awaiting_model_config_options ||
		    session.current_model_id.empty() || session.current_model_id.size() > 1537 || !CompatibleImageVersion(session.agent_version) ||
		    (!settings.model_id.empty() && settings.model_id != session.current_model_id)) return {};
		const auto first = session.queued_prompt.find_first_not_of(" \t\r\n");
		const auto authored_first = permission->content.find_first_not_of(" \t\r\n");
		if (first == std::string::npos || session.queued_prompt[first] == '/' ||
		    (authored_first != std::string::npos && permission->content[authored_first] == '/')) return {};
		const auto& state = *companion->state;
		if (!state.captured || !state.ready || state.failed || state.failures != 0 || state.query.host || state.lifetime.empty() ||
		    state.identity.chat != session.chat_id || state.identity.native != session.session_id || state.identity.process != session.last_process_id ||
		    state.identity.host != session.process_execution_host_id || state.identity.control != session.remote_output_delivery_token ||
		    state.identity.incarnation != permission->incarnation || state.identity.workspace != settings.workspace_directory ||
		    state.query.workspace != settings.workspace_directory || state.query.endpoint.port != companion->endpoint.port || state.query.endpoint.token != companion->endpoint.token ||
		    !opencode_native::ValidEndpoint(companion->endpoint)) return {};
		// The global image bit is insufficient: retain the actual current ACP selection receipt.
		std::size_t model_receipts = 0, mode_receipts = 0;
		Json controls = Json::array();
		std::set<std::string> control_ids;
		if (session.available_config_options.size() > 128) return {};
		for (const auto& option : session.available_config_options)
		{
			if (option.id.empty() || option.id.size() > 512 || option.current_value.size() > 2048 || !control_ids.insert(option.id).second) return {};
			if (option.id == "model" || option.category == "model")
			{
				++model_receipts;
				if (option.current_value != session.current_model_id) return {};
			}
			if (option.id == "mode" || option.category == "mode") ++mode_receipts;
			controls.push_back({option.id, option.category, option.current_value});
		}
		if (model_receipts != 1 || mode_receipts != 1 || session.current_mode_id.empty()) return {};
		std::vector<MessageAttachment> declared;
		for (const auto& attachment : permission->attachments) if (attachment.kind == "image") declared.push_back(attachment);
		if (declared != images) return {};
		for (const auto& [id, method] : session.pending_request_methods)
			if (method == "session/set_model" || method == "session/set_mode" || method == "session/set_config_option") return {};
		ImageOwner owner;
		owner.permission = permission; owner.companion = companion->state.get(); owner.lifetime = state.lifetime;
		owner.process = session.last_process_id; owner.native = session.session_id; owner.workspace = settings.workspace_directory;
		owner.endpoint_port = companion->endpoint.port; owner.endpoint_token = companion->endpoint.token; owner.control = session.remote_output_delivery_token;
		owner.turn = session.turn_serial; owner.first = session.turn_first_user_message_index; owner.last = session.turn_user_message_index;
		owner.images = images;
		owner.settings = Json::array({settings.native_session_id, settings.execution_host_id, settings.model_id, settings.reasoning_effort,
		    settings.service_tier, settings.service_tier_explicit, settings.approval_mode, settings.command_safety_tier});
		owner.receipts = Json::array({session.agent_name, session.agent_version, session.current_model_id, session.current_mode_id, controls,
		    session.goal_turn_model_id, session.goal_command_revision, session.goal_review_turn, session.active_uam_agent_id,
		    session.active_uam_agent_definition_hash, session.active_uam_agent_execution_capability, session.active_uam_agent_workspace_access,
		    session.queued_prompt});
		return owner;
	}
	ChatSession ImageSettings(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		ChatSession settings;
		settings.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
		settings.execution_host_id = chat.execution_host_id; settings.native_session_id = chat.native_session_id;
		settings.model_id = session.goal_turn_model_id.empty() ? chat.model_id : session.goal_turn_model_id;
		settings.reasoning_effort = chat.reasoning_effort; settings.service_tier = chat.service_tier;
		settings.service_tier_explicit = chat.service_tier_explicit; settings.approval_mode = chat.approval_mode; settings.command_safety_tier = chat.command_safety_tier;
		return settings;
	}
	bool ImageSourceMatches(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const ImageOwner& expected)
	{
		if (!expected.permission || expected.permission->root != app.data_root || !acp_detail::SavedImageRetrySourceMatches(app, session, chat, *expected.permission)) return false;
		const auto current = CaptureImageOwner(session, ImageSettings(app, session, chat), expected.images);
		return current && *current == expected;
	}
}

OpenCodeNativeGateState OpenCodeNativeActivityService::PrepareSavedImageRetry(AcpSessionState& session, const ChatSession& settings,
    const std::vector<MessageAttachment>& images, std::string& error)
{
	const auto captured = CaptureImageOwner(session, settings, images);
	if (!captured)
	{
		error = "OpenCode saved-image Retry requires the owned local session, acknowledged model settings and a supported ordinary prompt. The prompt was not sent.";
		if (session.opencode_native_companion && session.opencode_native_companion->state && session.opencode_native_companion->state->image)
			RejectImage(*session.opencode_native_companion->state->image, "OpenCode image ownership changed. Retry the saved message again.");
		return OpenCodeNativeGateState::Failed;
	}
	auto& state = *session.opencode_native_companion->state;
	if (!state.image || state.image->owner.permission != captured->permission)
	{
		if (state.image_generation == std::numeric_limits<std::uint64_t>::max())
		{ error = "OpenCode image verification must reconnect before retrying."; return OpenCodeNativeGateState::Failed; }
		ImageEvidence image;
		image.owner = *captured; image.generation = ++state.image_generation;
		image.deadline = Clock::now() + std::chrono::seconds(10);
		state.image = std::move(image);
	}
	if (!(state.image->owner == *captured)) RejectImage(*state.image, "OpenCode image ownership or model settings changed. Retry the saved message again.");
	if (state.image->service_cancel.stop_requested()) RejectImage(*state.image, "OpenCode image verification was stopped. Retry the saved message again.");
	if (state.image->phase == ImageEvidence::Phase::Pending && Clock::now() >= state.image->deadline)
		RejectImage(*state.image, "OpenCode image verification timed out. Retry the saved message again.");
	if (state.image->phase == ImageEvidence::Phase::Rejected)
	{ error = state.image->result.error; return OpenCodeNativeGateState::Failed; }
	return state.image->phase == ImageEvidence::Phase::Ready ? OpenCodeNativeGateState::Ready : OpenCodeNativeGateState::Pending;
}

Json OpenCodeNativeActivityService::SavedImageRetryMetadata(const AcpSessionState& session, const ChatSession& settings)
{
	if (!session.opencode_native_companion || !session.opencode_native_companion->state) return {{"phase", "invalid"}};
	const auto& state = *session.opencode_native_companion->state;
	if (!state.image || state.image->service_cancel.stop_requested() || state.image->phase != ImageEvidence::Phase::Ready || !state.image->result.ok) return {{"phase", "invalid"}};
	const auto owner = CaptureImageOwner(session, settings, state.image->owner.images);
	if (!owner || !(*owner == state.image->owner)) return {{"phase", "invalid"}};
	Json names = Json::array();
	for (const auto& image : owner->images) names.push_back(image.name);
	// No endpoint credential, configured provider options or raw catalog leaves typed state.
	return {{"phase", "ready"}, {"lifetime", owner->lifetime}, {"generation", state.image->generation},
	    {"sessionId", owner->native}, {"names", std::move(names)}, {"facts", state.image->result.facts}};
}

bool OpenCodeNativeActivityService::TaskApprovalEnabled(const Json& session)
{
	const auto rules = session.find("permission");
	if (rules == session.end() || !rules->is_array()) return false;
	std::string action;
	for (const Json& rule : *rules)
		if (rule.is_object() && String(rule, "permission") == "task" && String(rule, "pattern") == "*") action = String(rule, "action");
	return action == "ask";
}

bool OpenCodeNativeActivityService::OwnsSession(const std::string& root, const std::string& workspace, const std::string& candidate, const Json& sessions, std::string_view platform)
{
	if (!opencode_native::ValidNativeId(root) || !sessions.is_object()) return false;
	std::string current = candidate;
	std::set<std::string> visited;
	for (std::size_t depth = 0; depth < kMaximumAncestry; ++depth)
	{
		if (!opencode_native::ValidNativeId(current) || !visited.insert(current).second) return false;
		const auto found = sessions.find(current);
		if (found == sessions.end() || !found->is_object() || String(*found, "id") != current || !opencode::WorkspaceDirectoriesMatch(workspace, String(*found, "directory"), platform)) return false;
		if (current == root) return true;
		current = String(*found, "parentID");
	}
	return false;
}

std::optional<bool> OpenCodeNativeActivityService::TryWritePermissionResponse(AcpSessionState& session, const Json& message, std::string* error)
{
	const std::string id = String(message, "id");
	const auto fail = [&](const char* reason) -> std::optional<bool> { if (error) *error = reason; return false; };
	if (!id.starts_with(kRequestPrefix))
	{
		if (!session.opencode_native_companion || !session.opencode_native_companion->state) return {};
		const auto& state = *session.opencode_native_companion->state;
		if (state.ready && !state.failed && state.failures == 0) return {};
		const auto response_id = message.find("id");
		const auto& pending = session.pending_permission;
		if (response_id == message.end() || pending.request_id_json.empty() || response_id->dump() != pending.request_id_json) return {};
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
		const bool delegation = runtime.ProviderRecognizesSubagentTool(pending.title) ||
		    std::ranges::any_of(session.tool_calls, [&](const AcpToolCallState& tool) { return tool.id == pending.tool_call_id && tool.is_sub_agent; });
		if (!delegation) return {};
		const auto result = message.find("result");
		if (result == message.end() || !result->is_object()) return {};
		const auto outcome = result->find("outcome");
		if (outcome == result->end() || !outcome->is_object() || String(*outcome, "outcome") != "selected") return {};
		for (const auto& option : pending.options)
			if (option.id == String(*outcome, "optionId") && !acp_detail::IsRejectPermissionOption(option.id, option.name, option.kind))
				return fail("Native OpenCode delegation is waiting for its permission observer to recover; this approval was not sent.");
		return {};
	}
	if (!session.opencode_native_companion || !session.opencode_native_companion->state) return fail("The native OpenCode permission companion is unavailable.");
	auto& state = *session.opencode_native_companion->state;
	const std::string native_id = id.substr(kRequestPrefix.size());
	const auto permission = state.permissions.find(native_id);
	if (!state.ready || state.failed || state.identity.native != session.session_id || state.identity.process != session.last_process_id ||
	    state.identity.host != session.process_execution_host_id || permission == state.permissions.end())
		return fail("The native OpenCode permission no longer belongs to this running session.");
	std::string reply = "reject";
	const auto result = message.find("result");
	if (result != message.end() && result->is_object())
	{
		const auto outcome = result->find("outcome");
		if (outcome != result->end() && outcome->is_object() && String(*outcome, "outcome") == "selected" && String(*outcome, "optionId") == "allow-once") reply = "once";
	}
	if (reply == "once" && (state.read_only || session.current_mode_id == "plan" || session.goal_review_turn || session.active_uam_agent_workspace_access == "read") && !AllowedUnderReadCeiling(permission->second))
		return fail("Plan and read-only restrictions prohibit this native child action.");
	if (state.replies.size() >= kMaximumPermissions && !state.replies.contains(native_id)) return fail("The native OpenCode permission reply queue is full.");
	state.replies[native_id] = reply;
	state.next = {};
	return true;
}

OpenCodeNativeGateState OpenCodeNativeActivityService::BeforeDispatch(AppState& app, AcpSessionState& session, ChatSession& chat, std::string& error)
{
	if (session.provider_id != "opencode-cli" || session.model_discovery_only || chat.imported_read_only ||
	    session.active_uam_agent_execution_capability == "opencode-native-agent-config") return OpenCodeNativeGateState::NotApplicable;
	if (!session.opencode_native_companion || !session.opencode_native_companion->state)
	{
		// Older running providers retain their launch-time task deny; their ordinary ACP prompts remain usable.
		return OpenCodeNativeGateState::NotApplicable;
	}
	auto& state = *session.opencode_native_companion->state;
	if (m_state->stopping || !session.running || !session.session_ready || session.last_process_id.empty() || session.session_id.empty() ||
	    session.chat_id != chat.id || chat.side_cleanup_requested || session.local_stop_pending || session.remote_stop_pending)
	{ error = "The owned native OpenCode session is not ready."; return OpenCodeNativeGateState::Failed; }
	Identity identity{chat.id, session.session_id, session.last_process_id, paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)), session.process_execution_host_id, session.remote_output_delivery_token, chat.metadata_incarnation};
	if (!state.captured)
	{
		state.identity = identity;
		state.query.endpoint = session.opencode_native_companion->endpoint;
		state.query.workspace = identity.workspace;
		state.query.process_id = "acp-" + chat.id;
		state.query.control_token = identity.control;
		if (!paths::IsControllerLocalWorkspace(chat))
		{
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			if (!host || host->transport != "ssh" || host->runner_protocol_version < 3 || identity.control.empty() || identity.host != host->id)
			{ error = "The native OpenCode companion requires the owned protocol 3 SSH runner."; return OpenCodeNativeGateState::Failed; }
			state.query.host = *host;
		}
		state.captured = true;
	}
	if (!(state.identity == identity)) { error = "The native OpenCode process authority changed."; return OpenCodeNativeGateState::Failed; }
	if (state.query.host)
	{
		const ExecutionHost* current = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		if (!current || !execution_hosts::SameConnection(*state.query.host, *current))
		{
			state.failed = true;
			state.error = "The native OpenCode execution host changed while its process was running.";
		}
	}
	const bool read_only = session.goal_review_turn || session.active_uam_agent_workspace_access == "read" || approval_modes::AppApprovalModeOrEmpty(chat.approval_mode) == approval_modes::kPlanApprovalMode;
	if (state.read_only != read_only)
	{
		if (read_only && state.ready && (session.prompt_request_id != 0 || session.recovering_remote_turn))
		{
			state.failed = true;
			state.error = "The native OpenCode delegation policy changed during an active turn.";
		}
		state.read_only = read_only;
		state.ready = false;
		state.next = {};
	}
	if (state.failed) { error = state.error; return OpenCodeNativeGateState::Failed; }
	if (state.failures > 0)
	{
		error = "Native OpenCode permission observation is retrying; the next prompt has not been sent.";
		return OpenCodeNativeGateState::Pending;
	}
	return state.ready ? OpenCodeNativeGateState::Ready : OpenCodeNativeGateState::Pending;
}

bool OpenCodeNativeActivityService::Poll(AppState& app)
{
	if (m_state->stopping) return false;
	bool changed = false;
	for (auto iterator = m_state->image_jobs.begin(); iterator != m_state->image_jobs.end();)
	{
		const auto& owner = iterator->owner;
		const ChatSession* chat = ChatDomainService().FindChatById(app, iterator->identity.permission->chat_id);
		const bool current = chat && owner->image && owner->image->generation == iterator->generation &&
		    owner->image->phase == ImageEvidence::Phase::Pending && Clock::now() < owner->image->deadline &&
		    std::ranges::any_of(app.acp_sessions, [&](const auto& candidate)
		    {
			    return candidate && candidate->opencode_native_companion && candidate->opencode_native_companion->state == owner &&
			        ImageSourceMatches(app, *candidate, *chat, iterator->identity);
		    });
		if (!current)
		{
			iterator->stop.request_stop();
			if (owner->image && owner->image->generation == iterator->generation && owner->image->phase != ImageEvidence::Phase::Rejected)
			{ RejectImage(*owner->image, "OpenCode image verification expired or lost its saved message. Retry the saved message again."); changed = true; }
		}
		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++iterator; continue; }
		ImageResult result;
		try { result = iterator->future.get(); } catch (...) { result.error = "OpenCode image verification failed. The prompt was not sent."; }
		if (owner->image && owner->image->generation == iterator->generation)
		{
			owner->image->in_flight = false;
			if (current)
			{
				owner->image->phase = result.ok ? ImageEvidence::Phase::Ready : ImageEvidence::Phase::Rejected;
				owner->image->result = std::move(result); changed = true;
			}
		}
		iterator = m_state->image_jobs.erase(iterator);
	}
	for (auto iterator = m_state->jobs.begin(); iterator != m_state->jobs.end();)
	{
		const ChatSession* current_chat = ChatDomainService().FindChatById(app, iterator->identity.chat);
		const bool still_owned = current_chat && current_chat->metadata_incarnation == iterator->identity.incarnation &&
		    std::ranges::any_of(app.acp_sessions, [&](const auto& candidate)
		    {
			    return candidate && candidate->running && candidate->opencode_native_companion && candidate->opencode_native_companion->state == iterator->owner &&
			        candidate->session_id == iterator->identity.native && candidate->last_process_id == iterator->identity.process && candidate->process_execution_host_id == iterator->identity.host;
		    });
		if (!still_owned) iterator->stop.request_stop();
		if (iterator->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++iterator; continue; }
		Snapshot result;
		try { result = iterator->future.get(); } catch (...) { result.error = "The native OpenCode permission worker failed."; }
		auto owner = iterator->owner;
		owner->in_flight = false;
		if (still_owned && owner->identity == iterator->identity && owner->read_only == iterator->read_only)
		{
			owner->next = Clock::now() + std::chrono::milliseconds(500);
			if (!result.ok)
			{
				owner->error = result.error.empty() ? "The authenticated native OpenCode permission observer failed." : result.error;
				// A missing snapshot is not evidence that the provider or a pending
				// decision failed. Hold new prompts and retry the complete observation;
				// queued decisions remain gated by fresh ownership proof before delivery.
				if (result.authority_conflict || ++owner->failures >= kConsecutiveFailureLimit) owner->failed = true;
			}
			else
			{
				owner->retry_recovered = owner->failures > 0;
				owner->failures = 0; owner->ready = true;
				if (ChatSession* chat = ChatDomainService().FindChatById(app, iterator->identity.chat); chat &&
				    CanRetireDecisions(app, *chat, owner->query, iterator->identity.native, iterator->identity.host, iterator->identity.control))
				{
					const auto previous = chat->remote_interaction_responses;
					std::erase_if(chat->remote_interaction_responses, [&](const AcpRemoteInteractionResponseState& saved)
					{
						const auto id = SavedDecisionId(saved);
						if (!id || owner->unowned_decisions.contains(DecisionKey(saved))) return false;
						if (result.pending_ids.contains(*id) && !result.owned_ids.contains(*id))
						{
							owner->unowned_decisions.insert(DecisionKey(saved));
							return false;
						}
						// A complete authenticated snapshot also settles a receipt whose native
						// request disappeared before controller restart or a failed cleanup save.
						return result.confirmed_replies.contains(*id) || !result.pending_ids.contains(*id);
					});
					if (previous.size() != chat->remote_interaction_responses.size() && !acp_detail::SaveChatQuietly(app, *chat))
						chat->remote_interaction_responses = previous;
				}
				for (const auto& [id, reply] : iterator->replies) { owner->replies.erase(id); owner->permissions.erase(id); }
				owner->permissions.clear();
				for (const Json& permission : result.permissions) owner->permissions.emplace(String(permission, "id"), permission);
				std::erase_if(owner->published, [&](const std::string& id) { return !owner->permissions.contains(id); });
			}
			changed = true;
		}
		iterator = m_state->jobs.erase(iterator);
	}
	// Requested image evidence gets first use of free slots, ahead of recurring permission snapshots.
	for (const auto& session_pointer : app.acp_sessions)
	{
		if (!session_pointer || !session_pointer->opencode_native_companion || !session_pointer->opencode_native_companion->state) continue;
		auto owner = session_pointer->opencode_native_companion->state;
		if (!owner->image || owner->image->phase == ImageEvidence::Phase::Rejected) continue;
		auto& image = *owner->image;
		const auto* chat = ChatDomainService().FindChatById(app, session_pointer->chat_id);
		if (!chat || !ImageSourceMatches(app, *session_pointer, *chat, image.owner) ||
		    (image.phase == ImageEvidence::Phase::Pending && Clock::now() >= image.deadline))
		{
			RejectImage(image, "OpenCode image ownership changed or verification expired. Retry the saved message again.");
			for (auto& job : m_state->image_jobs) if (job.owner == owner && job.generation == image.generation) job.stop.request_stop();
			changed = true; continue;
		}
		if (image.phase != ImageEvidence::Phase::Pending || image.in_flight || m_state->jobs.size() + m_state->image_jobs.size() >= kMaximumWorkers) continue;
		State::ImageJob job;
		job.owner = owner; job.identity = image.owner; job.generation = image.generation;
		const Query base = owner->query;
		const auto execute = m_state->execute;
		const auto stop = job.stop.get_token();
		const auto model = session_pointer->current_model_id, version = session_pointer->agent_version;
		std::packaged_task<ImageResult()> task([base, model, version, execute, stop]()
		{ return ReadImageEvidence(base, model, version, execute, stop); });
		job.future = task.get_future();
		m_state->image_jobs.push_back(std::move(job));
		image.service_cancel = m_state->image_stop.get_token();
		image.in_flight = true;
		try { std::thread(std::move(task)).detach(); }
		catch (...)
		{
			m_state->image_jobs.back().stop.request_stop(); m_state->image_jobs.pop_back();
			image.in_flight = false;
			RejectImage(image, "OpenCode image verification could not start. The prompt was not sent."); changed = true;
		}
	}
	for (const auto& session_pointer : app.acp_sessions)
	{
		if (!session_pointer) continue;
		AcpSessionState& session = *session_pointer;
		ChatSession* chat = ChatDomainService().FindChatById(app, session.chat_id);
		if (!chat || !session.opencode_native_companion || !session.opencode_native_companion->state) continue;
		std::string gate_error;
		const auto gate = BeforeDispatch(app, session, *chat, gate_error);
		auto owner = session.opencode_native_companion->state;
		if (gate == OpenCodeNativeGateState::NotApplicable) continue;
		if (gate == OpenCodeNativeGateState::Failed)
		{
			if (owner->failed && session.running)
			{
				// Without this entry a UAM-initiated stop leaves the child timeline ending at session/new.
				acp_detail::AppendAcpDiagnostic(session, "native_companion", owner->failures >= kConsecutiveFailureLimit ? "observation_failures" : "ownership_conflict",
				    "", "", false, 0, gate_error, "failures=" + std::to_string(owner->failures));
				(void)StopAcpSession(app, session.chat_id, AcpStopPurpose::Interrupt, true);
				acp_detail::FailAcpTurnOrSession(session, chat, gate_error + " UAM stopped sending approvals and requested the provider stop.");
				acp_detail::SaveChatQuietly(app, *chat);
				changed = true;
			}
			continue;
		}
		if (owner->retry_recovered)
		{
			owner->retry_recovered = false;
			if (ProviderRuntimeRegistry::ResolveById(session.provider_id).ProviderRecognizesSubagentTool(session.pending_permission.title))
			{
				const std::string pending_id = session.pending_permission.request_id_json;
				const auto saved = std::ranges::find(chat->remote_interaction_responses, pending_id, &AcpRemoteInteractionResponseState::request_id_json);
				bool admitted = false;
				if (CanRetireDecisions(app, *chat, owner->query, owner->identity.native, owner->identity.host, owner->identity.control) && saved != chat->remote_interaction_responses.end())
				{
					const Json response = Json::parse(saved->response_json, nullptr, false);
					if (response.is_object() && response.contains("id") && response["id"].dump() == pending_id && response.contains("result") && response["result"].is_object())
					{
						const Json outcome = response["result"].value("outcome", Json::object());
						const std::string option = String(outcome, "optionId");
						if (String(outcome, "outcome") == "selected" && std::ranges::any_of(session.pending_permission.options, [&](const AcpPermissionOptionState& candidate) { return candidate.id == option; }))
						{
							admitted = true;
							// SSH admission already durably chose this exact response before its
							// temporary observer gate. Resume the same decision, never a new one.
							(void)ResolveAcpPermission(app, session.chat_id, pending_id, option, false);
						}
					}
				}
				if (!admitted) (void)acp_detail::TryAutoApprovePendingPermission(app, session, *chat);
			}
		}
		for (const auto& [id, permission] : owner->permissions)
		{
			if (!owner->published.insert(id).second) continue;
			const std::string native_session = String(permission, "sessionID");
			const std::string name = String(permission, "permission");
			Json input = permission.value("metadata", Json::object());
			if (!input.is_object()) input = Json::object();
			input["nativeSessionId"] = native_session; input["permission"] = name;
			input["patterns"] = permission.value("patterns", Json::array());
			Json request = {{"jsonrpc", "2.0"}, {"id", std::string(kRequestPrefix) + id}, {"method", "opencode/nativePermission"}, {"params", {
			    {"toolCall", {{"toolCallId", "opencode-child-permission:" + id}, {"title", name == "doom_loop" ? name : "Child agent: " + name}, {"kind", ReadPermission(name) ? "read" : name == "edit" ? "edit" : "other"}, {"status", "pending"}, {"rawInput", input}}},
			    {"options", Json::array({{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}}, {{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}})}}}};
			if (!acp_detail::ReplayPersistedInteractionResponseIfMatched(app, session, *chat, request))
				acp_detail::HandlePermissionRequest(app, session, *chat, request);
			changed = true;
		}
		// Native Task normally completes with its parent. Retained owned child activity keeps
		// the relay alive when a provider emits a background task beyond the parent turn.
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(session.provider_id);
		const bool child_tool_pending = std::ranges::any_of(session.tool_calls, [&](const AcpToolCallState& tool)
		{
			return (tool.is_sub_agent || runtime.ProviderRecognizesSubagentTool(tool.title)) &&
			    (acp_statuses::IsActiveStatus(tool.status) || acp_statuses::IsActiveStatus(tool.child_activity));
		});
		const bool receipt_pending = CanRetireDecisions(app, *chat, owner->query, owner->identity.native, owner->identity.host, owner->identity.control) &&
		    std::ranges::any_of(chat->remote_interaction_responses, [&](const AcpRemoteInteractionResponseState& saved)
		    { return SavedDecisionId(saved).has_value() && !owner->unowned_decisions.contains(DecisionKey(saved)); });
		const bool observe = !owner->ready || owner->failures > 0 || session.processing || session.prompt_request_id != 0 || session.recovering_remote_turn ||
		    session.waiting_for_permission || session.waiting_for_user_input || !owner->permissions.empty() || !owner->replies.empty() ||
		    child_tool_pending || runtime.AcpBackgroundTaskCount(session) > 0 || receipt_pending;
		if (!observe) continue;
		if (owner->in_flight || Clock::now() < owner->next || m_state->jobs.size() + m_state->image_jobs.size() >= kMaximumWorkers) continue;
		State::Job job;
		job.owner = owner; job.identity = owner->identity; job.replies = owner->replies; job.read_only = owner->read_only;
		const Query base = owner->query;
		const std::string root = owner->identity.native;
		const bool bootstrap = !owner->ready;
		const bool read_only = owner->read_only;
		const auto replies = job.replies;
		const auto expected_permissions = owner->permissions;
		const auto execute = m_state->execute;
		const bool default_execution = m_state->default_execution;
		const auto stop = job.stop.get_token();
		std::packaged_task<Snapshot()> task([base, root, bootstrap, read_only, replies, expected_permissions, execute, default_execution, stop]() mutable
		{
			Snapshot snapshot;
			const auto started = Clock::now();
			const auto budget = std::chrono::seconds(base.host ? 60 : 10);
			std::size_t queries = 0;
			// The complete snapshot owns one SSH bridge, including ancestry and reply RPCs.
			// Its destructor runs on this worker and never on the UI thread.
			std::unique_ptr<remote::RunnerClient> runner;
			if (default_execution && base.host)
			{
				const ExecutionHost& host = *base.host;
				runner = std::make_unique<remote::RunnerClient>(PlatformServicesFactory::Instance().process_service,
				    host);
				runner->SetProcessControlToken(base.process_id, base.control_token);
			}
			const auto query = [&](const Json& operation, Json& result)
			{
				if (stop.stop_requested() || ++queries > kMaximumSnapshotQueries || Clock::now() - started > budget)
				{ snapshot.error = "The bounded native OpenCode permission snapshot was interrupted."; return false; }
				Query request = base; request.operation = operation;
				if (runner) return runner->QueryOpenCodeNative(base.process_id, operation, result, &snapshot.error, stop);
				return execute(request, result, snapshot.error, stop);
			};
			Json native_root, permissions;
			if (!query({{"operation", "session"}, {"nativeSessionId", root}}, native_root)) return snapshot;
			std::string verified_workspace = base.workspace;
			const std::string_view platform = base.host ? std::string_view(base.host->platform) : std::string_view{};
			if (!base.host)
			{
				std::error_code expected_error, actual_error;
				const auto expected = std::filesystem::canonical(paths::PathFromUtf8(base.workspace), expected_error);
				const auto actual = std::filesystem::canonical(paths::PathFromUtf8(String(native_root, "directory")), actual_error);
				if (expected_error || actual_error || expected != actual)
				{ snapshot.authority_conflict = !expected_error && !actual_error; snapshot.error = "The native OpenCode root session did not match its owned workspace."; return snapshot; }
				verified_workspace = String(native_root, "directory");
			}
			snapshot.sessions[root] = native_root;
			if (!OwnsSession(root, verified_workspace, root, snapshot.sessions, platform))
			{
				snapshot.authority_conflict = (!String(native_root, "id").empty() && String(native_root, "id") != root) ||
				    (!String(native_root, "directory").empty() && String(native_root, "directory") != verified_workspace);
				snapshot.error = "The native OpenCode root session did not match its owned workspace.";
				return snapshot;
			}
			if (!query({{"operation", "permissions"}}, permissions)) return snapshot;
			if (!permissions.is_array() || permissions.size() > kMaximumPermissions)
			{ snapshot.error = "The native OpenCode pending permission snapshot was unavailable or exceeded its bound."; return snapshot; }
			if (bootstrap)
			{
				Json confirmed;
				if (!query({{"operation", read_only ? "disableTask" : "enableTask"}, {"nativeSessionId", root}, {"readOnly", read_only}}, confirmed)) return snapshot;
				if (!query({{"operation", "session"}, {"nativeSessionId", root}}, native_root)) return snapshot;
				std::string task_action;
				const auto rules = native_root.find("permission");
				if (rules != native_root.end() && rules->is_array())
					for (const Json& rule : *rules)
						if (String(rule, "permission") == "task" && String(rule, "pattern") == "*") task_action = String(rule, "action");
				if (task_action != (read_only ? "deny" : "ask"))
				{ snapshot.error = "The native OpenCode root did not confirm its child delegation policy."; return snapshot; }
				snapshot.sessions[root] = native_root;
				if (!OwnsSession(root, verified_workspace, root, snapshot.sessions, platform))
				{
					snapshot.authority_conflict = (!String(native_root, "id").empty() && String(native_root, "id") != root) ||
					    (!String(native_root, "directory").empty() && String(native_root, "directory") != verified_workspace);
					snapshot.error = "The native OpenCode root changed during its delegation policy readback.";
					return snapshot;
				}
			}
			const auto load_ancestry = [&](std::string cursor)
			{
				std::set<std::string> visited;
				for (std::size_t depth = 0; depth < kMaximumAncestry && cursor != root && opencode_native::ValidNativeId(cursor) && visited.insert(cursor).second; ++depth)
				{
					if (!snapshot.sessions.contains(cursor))
					{
						Json ancestor;
						if (!query({{"operation", "session"}, {"nativeSessionId", cursor}}, ancestor)) return false;
						snapshot.sessions[cursor] = std::move(ancestor);
					}
					cursor = String(snapshot.sessions[cursor], "parentID");
				}
				return true;
			};
			if (bootstrap && read_only)
			{
				Json statuses;
				if (!query({{"operation", "status"}}, statuses)) return snapshot;
				if (!statuses.is_object() || statuses.size() > kMaximumPermissions)
				{ snapshot.error = "The native OpenCode child activity snapshot was unavailable."; return snapshot; }
				for (const auto& [id, status] : statuses.items())
				{
					if (id == root || String(status, "type") == "idle") continue;
					if (!load_ancestry(id)) return snapshot;
					if (OwnsSession(root, verified_workspace, id, snapshot.sessions, platform))
					{ snapshot.authority_conflict = true; snapshot.error = "A native OpenCode child was active when entering Plan or read-only mode."; return snapshot; }
				}
			}
			for (const Json& permission : permissions)
			{
				const std::string id = String(permission, "id"), candidate = String(permission, "sessionID");
				if (!opencode_native::ValidNativeId(id) || !opencode_native::ValidNativeId(candidate) || String(permission, "permission").empty())
				{ snapshot.error = "The native OpenCode pending permission snapshot was malformed."; return snapshot; }
				if (!snapshot.pending_ids.insert(id).second)
				{ snapshot.error = "The native OpenCode pending permission snapshot repeated an identity."; return snapshot; }
				if (candidate == root) continue; // The ACP adapter already owns root callbacks.
				if (!load_ancestry(candidate)) return snapshot;
				if (OwnsSession(root, verified_workspace, candidate, snapshot.sessions, platform))
				{ snapshot.permissions.push_back(permission); snapshot.owned_ids.insert(id); }
			}
			for (const auto& [id, reply] : replies)
			{
				const auto pending = std::find_if(snapshot.permissions.begin(), snapshot.permissions.end(), [&](const Json& permission) { return String(permission, "id") == id; });
				if (pending == snapshot.permissions.end()) continue;
				const auto expected = expected_permissions.find(id);
				if (expected == expected_permissions.end() || *pending != expected->second || (read_only && reply == "once" && !AllowedUnderReadCeiling(*pending)))
				{ snapshot.authority_conflict = true; snapshot.error = "The native OpenCode permission changed before its decision could be delivered."; return snapshot; }
				Json confirmed;
				if (!query({{"operation", "reply"}, {"requestId", id}, {"reply", reply}}, confirmed)) return snapshot;
				if (confirmed != true)
				{ snapshot.error = "The native OpenCode server did not confirm the child permission decision."; return snapshot; }
				snapshot.confirmed_replies.insert(id);
			}
			snapshot.permissions.erase(std::remove_if(snapshot.permissions.begin(), snapshot.permissions.end(), [&](const Json& permission) { return replies.contains(String(permission, "id")); }), snapshot.permissions.end());
			if (stop.stop_requested() || Clock::now() - started > budget)
			{ snapshot.error = "The bounded native OpenCode permission snapshot was interrupted."; return snapshot; }
			snapshot.ok = true;
			return snapshot;
		});
		job.future = task.get_future();
		owner->in_flight = true;
		m_state->jobs.push_back(std::move(job));
		std::thread(std::move(task)).detach();
	}
	return changed;
}

bool OpenCodeNativeActivityService::HasPendingWork() const noexcept
{
	return m_state && (!m_state->jobs.empty() || !m_state->image_jobs.empty());
}

void OpenCodeNativeActivityService::Stop()
{
	if (!m_state || m_state->stopping) return;
	m_state->stopping = true;
	m_state->image_stop.request_stop();
	for (auto& job : m_state->jobs) { job.stop.request_stop(); job.owner->in_flight = false; job.owner->failed = true; job.owner->error = "The native OpenCode companion is stopping."; }
	m_state->jobs.clear();
	for (auto& job : m_state->image_jobs)
	{
		job.stop.request_stop();
		if (job.owner->image && job.owner->image->generation == job.generation) RejectImage(*job.owner->image, "OpenCode image verification was stopped. Retry the saved message again.");
	}
	m_state->image_jobs.clear();
}
}
