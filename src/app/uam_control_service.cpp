#include "app/uam_control_service.h"

#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/chat_domain_service.h"
#include "app/goal_service.h"
#include "app/markdown_store_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/settings_normalization.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_profile_constants.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/security/command_safety.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxRequestBytes = 64U * 1024U;
		constexpr std::size_t kMaxResponseBytes = 64U * 1024U;
		constexpr std::size_t kMaxObjectiveBytes = 32U * 1024U;
		constexpr std::size_t kMaxTaskBytes = 128U * 1024U;
		constexpr std::size_t kMaxSkillBytes = 128U * 1024U;
		constexpr std::size_t kMaxRequestsPerMinute = 30;
		constexpr std::size_t kMaxQueuedRequests = 32;
		constexpr std::size_t kMaxSeenRequests = 256;
		constexpr std::size_t kMaxAuditRecords = 64;
		constexpr std::size_t kMaxProcessedPerTick = 16;
		constexpr int64_t kCapabilityLifetimeMs = 24LL * 60LL * 60LL * 1000LL;
		constexpr int64_t kModelGoalTokenBudget = 200000;
		constexpr std::string_view kServerFlag = "--uam-control-mcp";
		constexpr const char* kCapabilityDirectoryEnvironment = "UAM_CONTROL_CAPABILITY_DIR";

		struct ToolResult
		{
			bool ok = false;
			nlohmann::json result = nlohmann::json::object();
			std::string error;
			std::string reason;
		};

		std::filesystem::path ControlRoot(const AppState& app)
		{
			return app.data_root / "uam-control";
		}

		std::string NewUuid()
		{
			return PlatformServicesFactory::Instance().process_service.GenerateUuid();
		}

		std::string RandomRequestId()
		{
			std::random_device random;
			std::ostringstream out;
			for (int index = 0; index < 4; ++index)
			{
				out << std::hex << std::setw(8) << std::setfill('0') << random();
			}
			return out.str();
		}

		bool SafeRequestId(std::string_view value)
		{
			return !value.empty() && value.size() <= 80 &&
			       std::ranges::all_of(value, [](unsigned char ch) {
				       return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
				              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
			       });
		}

		bool SafeText(std::string_view value, std::size_t limit)
		{
			return value.size() <= limit && value.find('\0') == std::string_view::npos;
		}

		bool HasOnlyStringArguments(const nlohmann::json& arguments,
		                            std::initializer_list<std::string_view> required,
		                            std::initializer_list<std::string_view> optional = {})
		{
			if (!arguments.is_object() || arguments.size() < required.size() ||
			    arguments.size() > required.size() + optional.size()) return false;
			for (std::string_view key : required)
			{
				const auto value = arguments.find(std::string(key));
				if (value == arguments.end() || !value->is_string()) return false;
			}
			for (auto value = arguments.begin(); value != arguments.end(); ++value)
			{
				const std::string_view key = value.key();
				if (!value->is_string() ||
				    (std::ranges::find(required, key) == required.end() &&
				     std::ranges::find(optional, key) == optional.end())) return false;
			}
			return true;
		}

		bool ValidToolArguments(std::string_view method, const nlohmann::json& arguments)
		{
			if (method == "skill_list" || method == "agent_list" || method == "goal_get")
				return arguments.is_object() && arguments.empty();
			if (method == "skill_read") return HasOnlyStringArguments(arguments, {"id"});
			if (method == "agent_status" || method == "agent_cancel")
				return HasOnlyStringArguments(arguments, {"runId"});
			if (method == "agent_delegate")
				return HasOnlyStringArguments(arguments, {"agentId", "task"}, {"providerId", "modelId"});
			if (method == "goal_create")
				return HasOnlyStringArguments(arguments, {"objective", "idempotencyKey"});
			return false;
		}

		bool ValidStdioToolCall(const nlohmann::json& request)
		{
			return request.is_object() && request.contains("method") && request["method"].is_string() &&
			       request["method"] == "tools/call" && request.contains("params") && request["params"].is_object() &&
			       request["params"].contains("name") && request["params"]["name"].is_string() &&
			       request["params"].contains("arguments") && request["params"]["arguments"].is_object();
		}

		bool Contains(const std::vector<std::string>& values, std::string_view value)
		{
			return std::ranges::find(values, value) != values.end();
		}

		bool IsMutation(std::string_view method)
		{
			return method == "agent_delegate" || method == "agent_cancel" || method == "goal_create";
		}

		bool MutationsAllowed(const ChatSession& chat)
		{
			return uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode) !=
			           uam::approval_modes::kPlanApprovalMode &&
			       uam::command_safety::ParseTier(chat.command_safety_tier) ==
			           uam::command_safety::Tier::Yolo;
		}

		UamControlCapability* FindCapability(AppState& app, std::string_view id)
		{
			const auto found = std::ranges::find(app.uam_control_capabilities, id, &UamControlCapability::id);
			return found == app.uam_control_capabilities.end() ? nullptr : &*found;
		}

		const AgentRun* FindRun(const AppState& app, std::string_view id)
		{
			const auto found = std::ranges::find(app.agent_runs, id, &AgentRun::id);
			return found == app.agent_runs.end() ? nullptr : &*found;
		}

		const AgentDefinition* FindDefinition(const AgentDefinitionCatalog& catalog, std::string_view id)
		{
			const auto found = std::ranges::find(catalog.definitions, id, &AgentDefinition::id);
			return found == catalog.definitions.end() ? nullptr : &*found;
		}

		bool IsOwnedDescendant(const AppState& app, const UamControlCapability& capability,
		                       const AgentRun& candidate)
		{
			if (candidate.root_chat_id != capability.chat_id) return false;
			if (capability.agent_run_id.empty())
			{
				const AgentRun* run = &candidate;
				while (run != nullptr)
				{
					if (capability.owned_agent_run_ids.contains(run->id)) return true;
					run = run->parent_run_id.empty() ? nullptr : FindRun(app, run->parent_run_id);
				}
				return false;
			}
			std::string parent = candidate.parent_run_id;
			while (!parent.empty())
			{
				if (parent == capability.agent_run_id) return true;
				const AgentRun* run = FindRun(app, parent);
				if (run == nullptr || run->root_chat_id != capability.chat_id) return false;
				parent = run->parent_run_id;
			}
			return false;
		}

		std::string GoalStatusName(GoalStatus status)
		{
			switch (status)
			{
				case GoalStatus::Active: return "active";
				case GoalStatus::Complete: return "complete";
				case GoalStatus::Blocked: return "blocked";
				case GoalStatus::Paused: return "paused";
			}
			return "paused";
		}

		nlohmann::json GoalJson(const Goal* goal, const AppState& app)
		{
			if (goal == nullptr) return nullptr;
			AppSettings bounded_settings = app.settings;
			uam::settings::ClampGoalSettings(bounded_settings);
			return {
			    {"id", goal->id}, {"objective", goal->objective}, {"status", GoalStatusName(goal->status)},
			    {"tokenBudget", goal->token_budget}, {"tokensUsed", goal->tokens_used},
			    {"maxLoopIterations", bounded_settings.goal_max_loop_iterations},
			    {"executionOwner", "uam"}, {"creator", goal->creator},
			    {"creatorProviderId", goal->creator_provider_id}, {"creatorAgentId", goal->creator_agent_id},
			    {"creatorRunId", goal->creator_run_id},
			};
		}

		std::string AuditText(std::string value)
		{
			value = uam::strings::SafeLine(value, 512, true);
			return value;
		}

		bool AppendAuditAndSave(AppState& app, const UamControlCapability& capability,
		                        std::string_view request_id, std::string_view method,
		                        const ToolResult& result)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (chat == nullptr) return false;
			chat->uam_control_audit.push_back({
			    .request_id = AuditText(std::string(request_id)),
			    .method = AuditText(std::string(method)),
			    .result = result.ok ? "ok" : "rejected",
			    .reason = AuditText(result.ok ? result.reason : result.error),
			    .provider_id = capability.provider_id,
			    .agent_id = capability.agent_id,
			    .run_id = capability.agent_run_id,
			    .created_at = uam::time::TimestampNow(),
			});
			if (chat->uam_control_audit.size() > kMaxAuditRecords)
			{
				chat->uam_control_audit.erase(chat->uam_control_audit.begin(),
				                              chat->uam_control_audit.end() - kMaxAuditRecords);
			}
			return ChatRepository::SaveChat(app.data_root, *chat);
		}

		AgentDefinitionCatalog AgentCatalog(const AppState& app, const ChatSession& chat)
		{
			return AgentDefinitionService::Load(
			    app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
		}

		std::vector<std::string> AllowedSkillIds(const UamControlCapability& capability)
		{
			return capability.agent_skills;
		}

		ToolResult SkillList(const AppState& app, const UamControlCapability& capability,
		                     const ChatSession& chat)
		{
			ToolResult out{.ok = true, .result = nlohmann::json::array(), .reason = "Listed approved skill metadata."};
			const std::vector<std::string> allowed = AllowedSkillIds(capability);
			if (allowed.empty()) return out;
			std::string error;
			const auto entries = MarkdownStoreService::ListEntries(
			    MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory), &error);
			if (!error.empty()) return {.error = error};
			for (const auto& entry : entries)
			{
				if (!Contains(allowed, entry.command_name)) continue;
				out.result.push_back({{"id", entry.command_name}, {"title", entry.title},
				                      {"preview", entry.preview}, {"review", entry.review},
				                      {"version", "fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(entry.body))}});
				if (out.result.size() == 16) break;
			}
			return out;
		}

		ToolResult SkillRead(const AppState& app, const UamControlCapability& capability,
		                     const ChatSession& chat, const nlohmann::json& arguments)
		{
			const std::string id = arguments.value("id", "");
			const std::vector<std::string> allowed = AllowedSkillIds(capability);
			if (!Contains(allowed, id)) return {.error = "Skill is not approved for this agent."};
			std::string error;
			const auto entries = MarkdownStoreService::ListEntries(
			    MarkdownStoreService::NormalizeRoot(app.settings.markdown_store_directory), &error);
			if (!error.empty()) return {.error = error};
			for (const auto& entry : entries)
			{
				if (entry.command_name != id) continue;
				if (!SafeText(entry.body, kMaxSkillBytes)) return {.error = "Skill snapshot exceeds the 128 KiB response ceiling."};
				return {.ok = true,
				        .result = {{"id", id}, {"title", entry.title}, {"body", entry.body},
				                   {"version", "fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(entry.body))}},
				        .reason = "Read one approved versioned skill snapshot."};
			}
			return {.error = "Approved skill snapshot is unavailable."};
		}

		ToolResult AgentList(const AppState& app, const UamControlCapability& capability,
		                     const ChatSession& chat)
		{
			ToolResult out{.ok = true, .result = nlohmann::json::array(), .reason = "Listed bounded delegate metadata."};
			const AgentDefinitionCatalog catalog = AgentCatalog(app, chat);
			for (const AgentDefinition& definition : catalog.definitions)
			{
				if (definition.mode != "subagent" && definition.mode != "both") continue;
				if (!capability.agent_run_id.empty() && !Contains(capability.agent_delegates, definition.id)) continue;
				const bool forced_read = definition.workspace_access == "read" ||
				                         (!capability.agent_run_id.empty() && FindRun(app, capability.agent_run_id) != nullptr &&
				                          FindRun(app, capability.agent_run_id)->effective_workspace_access == "read");
				out.result.push_back({{"id", definition.id}, {"description", definition.description},
				                      {"workspaceAccess", forced_read ? "read" : "write"},
				                      {"providerNeutral", true}});
				if (out.result.size() == 64) break;
			}
			return out;
		}

		ToolResult AgentDelegate(AppState& app, UamControlCapability& capability,
		                         const nlohmann::json& arguments)
		{
			const std::string agent_id = uam::strings::Trim(arguments.value("agentId", ""));
			const std::string task = uam::strings::Trim(arguments.value("task", ""));
			const std::string provider_id = uam::strings::Trim(arguments.value("providerId", ""));
			const std::string model_id = uam::strings::Trim(arguments.value("modelId", ""));
			if (!SafeText(task, kMaxTaskBytes)) return {.error = "Delegation task exceeds the 128 KiB ceiling."};
			std::string run_id;
			std::string error;
			if (!AgentRunScheduler::Enqueue(app, capability.chat_id, capability.agent_run_id,
			                                agent_id, task, &run_id, &error, provider_id, model_id))
				return {.error = std::move(error)};
			capability.owned_agent_run_ids.insert(run_id);
			return {.ok = true, .result = {{"runId", run_id}, {"status", "queued"}},
			        .reason = "Queued one bounded child run."};
		}

		ToolResult AgentStatus(const AppState& app, const UamControlCapability& capability,
		                       const nlohmann::json& arguments)
		{
			const std::string run_id = arguments.value("runId", "");
			const AgentRun* run = FindRun(app, run_id);
			if (run == nullptr || !IsOwnedDescendant(app, capability, *run))
				return {.error = "Agent run is outside this capability's descendant scope."};
			return {.ok = true,
			        .result = {{"runId", run->id}, {"status", run->status}, {"agentId", run->agent_id},
			                   {"providerId", run->provider_id}, {"modelId", run->model_id},
			                   {"executionCapability", run->execution_capability},
			                   {"resumedFromRunId", run->resumed_from_run_id},
			                   {"definitionHash", run->definition_hash},
			                   {"transcriptChatId", run->transcript_chat_id},
			                   {"result", run->result_excerpt}, {"diagnosticCode", run->diagnostic_code},
			                   {"diagnostic", run->diagnostic}},
			        .reason = "Read one owned descendant status."};
		}

		ToolResult AgentCancel(AppState& app, const UamControlCapability& capability,
		                       const nlohmann::json& arguments)
		{
			const std::string run_id = arguments.value("runId", "");
			const AgentRun* run = FindRun(app, run_id);
			if (run == nullptr || !IsOwnedDescendant(app, capability, *run))
				return {.error = "Agent run is outside this capability's descendant scope."};
			std::string error;
			if (!AgentRunScheduler::CancelTree(app, run_id, &error)) return {.error = std::move(error)};
			return {.ok = true, .result = {{"runId", run_id}, {"status", "cancelled"}},
			        .reason = "Cancelled one owned descendant tree."};
		}

		ToolResult GoalGet(const AppState& app, const UamControlCapability& capability)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			const bool mutations_allowed = chat != nullptr && MutationsAllowed(*chat);
			return {.ok = true,
			        .result = {{"goal", GoalJson(GoalService::FindActiveGoal(app, capability.chat_id), app)},
			                   {"controlEnabled", chat != nullptr && chat->uam_control_enabled},
			                   {"mutationToolsEnabled", mutations_allowed},
			                   {"mutationPolicy", mutations_allowed ? "yoloAutoApprove" : "readOnly"},
			                   {"agentGoalManagementEnabled", mutations_allowed}, {"lifecycleOwner", "uam"},
			                   {"modelMayCreateOnly", mutations_allowed}},
			        .reason = "Read current-chat goal state and immutable ceilings."};
		}

		ToolResult GoalCreate(AppState& app, UamControlCapability& capability,
		                      const nlohmann::json& arguments)
		{
			const std::string objective = uam::strings::Trim(arguments.value("objective", ""));
			const std::string idempotency_key = uam::strings::Trim(arguments.value("idempotencyKey", ""));
			if (objective.empty() || !SafeText(objective, kMaxObjectiveBytes))
				return {.error = "Goal objective must be between 1 byte and 32 KiB."};
			if (!SafeRequestId(idempotency_key)) return {.error = "Goal creation requires a bounded idempotency key."};
			const std::string idempotency_hash = "fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(idempotency_key));
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (chat == nullptr || std::ranges::any_of(chat->goals, [&](const Goal& goal) {
				    return goal.creator == "model" && goal.creator_request_key_hash == idempotency_hash;
			    }))
				return {.error = "Goal creation idempotency key was already used."};
			if (GoalService::FindActiveGoal(app, capability.chat_id) != nullptr)
				return {.error = "This chat already has an active goal."};
			std::string goal_id;
			if (!GoalService::CreateGoal(app, capability.chat_id, objective, kModelGoalTokenBudget,
			                             &goal_id, "uam", "", "model", capability.provider_id,
			                             capability.agent_id, capability.agent_run_id, idempotency_hash) ||
			    !GoalService::SetActiveGoal(app, capability.chat_id, goal_id))
				return {.error = "UAM could not create and activate the goal."};
			return {.ok = true, .result = {{"goalId", goal_id}, {"status", "active"},
			                              {"tokenBudget", kModelGoalTokenBudget}, {"lifecycleOwner", "uam"}},
			        .reason = "Created one visible bounded UAM-owned goal."};
		}

		ToolResult ExecuteTool(AppState& app, UamControlCapability& capability,
		                       std::string_view method, const nlohmann::json& arguments)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (chat == nullptr || !chat->uam_control_enabled)
				return {.error = "Agent-goal management is disabled or the owning chat is unavailable."};
			if (IsMutation(method) && !MutationsAllowed(*chat))
				return {.error = "UAM Control mutations require the owning chat's YOLO permission mode and are forbidden in Plan mode; this request was rejected without changing state."};
			if (method == "skill_list") return SkillList(app, capability, *chat);
			if (method == "skill_read") return SkillRead(app, capability, *chat, arguments);
			if (method == "agent_list") return AgentList(app, capability, *chat);
			if (method == "agent_delegate") return AgentDelegate(app, capability, arguments);
			if (method == "agent_status") return AgentStatus(app, capability, arguments);
			if (method == "agent_cancel") return AgentCancel(app, capability, arguments);
			if (method == "goal_get") return GoalGet(app, capability);
			if (method == "goal_create") return GoalCreate(app, capability, arguments);
			return {.error = "Unknown UAM control method."};
		}

		bool HasAuthority(const AppState& app, const UamControlCapability& capability,
		                  int64_t now_epoch_ms)
		{
			if (now_epoch_ms < 0 || now_epoch_ms >= capability.expires_at_epoch_ms ||
			    !uam::paths::IsDirectoryNoThrow(capability.directory) ||
			    uam::paths::IsLinkOrReparsePointNoThrow(capability.directory)) return false;
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value) {
				return value != nullptr && value->chat_id == capability.session_chat_id && value->running &&
				       value->uam_control_capability_id == capability.id && value->provider_id == capability.provider_id;
			});
			return session != app.acp_sessions.end();
		}

		nlohmann::json HandleRequest(AppState& app, const std::string& capability_id,
		                             const nlohmann::json& request, int64_t now_epoch_ms)
		{
			auto reject = [](std::string error) {
				return nlohmann::json{{"ok", false}, {"error", std::move(error)}};
			};
			if (!request.is_object() || request.dump().size() > kMaxRequestBytes)
				return reject("Malformed or oversized control request.");
			UamControlCapability* capability = FindCapability(app, capability_id);
			if (capability == nullptr || !HasAuthority(app, *capability, now_epoch_ms))
				return reject("Control capability is stale, expired, revoked, or outside its provider session.");

			if (request.size() != 3 || !request.contains("requestId") || !request["requestId"].is_string() ||
			    !request.contains("method") || !request["method"].is_string() ||
			    !request.contains("arguments") || !request["arguments"].is_object())
				return reject("Control request has an invalid shape or field type.");
			const std::string request_id = request["requestId"].get<std::string>();
			const std::string method = request["method"].get<std::string>();
			const nlohmann::json& arguments = request["arguments"];
			if (!SafeRequestId(request_id) || method.empty() || method.size() > 64 ||
			    !ValidToolArguments(method, arguments))
				return reject("Control request has an unknown method or invalid arguments.");
			if (!capability->seen_request_ids.insert(request_id).second)
				return reject("Control request replay was rejected.");
			if (capability->seen_request_ids.size() > kMaxSeenRequests)
				capability->seen_request_ids.erase(capability->seen_request_ids.begin());

			while (!capability->request_times_epoch_ms.empty() &&
			       capability->request_times_epoch_ms.front() <= now_epoch_ms - 60000)
				capability->request_times_epoch_ms.pop_front();
			if (capability->request_times_epoch_ms.size() >= kMaxRequestsPerMinute)
				return reject("Control capability exceeded the bounded request rate.");
			capability->request_times_epoch_ms.push_back(now_epoch_ms);

			const ChatSession* before_chat = ChatDomainService().FindChatById(app, capability->chat_id);
			const std::optional<ChatSession> rollback = before_chat == nullptr ? std::nullopt : std::optional<ChatSession>{*before_chat};
			ToolResult result = ExecuteTool(app, *capability, method, arguments);
			const UamControlCapability capability_snapshot = *capability;
			if (!AppendAuditAndSave(app, capability_snapshot, request_id, method, result))
			{
				if (result.ok && method == "agent_delegate")
				{
					std::string cancel_error;
					(void)AgentRunScheduler::CancelTree(app, result.result.value("runId", ""), &cancel_error);
				}
				if (rollback.has_value())
				{
					if (ChatSession* chat = ChatDomainService().FindChatById(app, capability_snapshot.chat_id); chat != nullptr)
						*chat = *rollback;
				}
				return reject("Control result could not be durably recorded.");
			}
			nlohmann::json response = result.ok
			                              ? nlohmann::json{{"ok", true}, {"result", std::move(result.result)}}
			                              : reject(std::move(result.error));
			if (response.dump().size() > kMaxResponseBytes)
				return reject("Control response exceeds the 64 KiB ceiling.");
			return response;
		}

		nlohmann::json ToolSchema(std::string name, nlohmann::json properties = nlohmann::json::object(),
		                          nlohmann::json required = nlohmann::json::array())
		{
			return {{"name", std::move(name)}, {"description", "Bounded UAM control operation."},
			        {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)},
			                         {"required", std::move(required)}, {"additionalProperties", false}}}};
		}

		nlohmann::json MutationToolSchema(std::string name, nlohmann::json properties,
		                                  nlohmann::json required)
		{
			nlohmann::json tool = ToolSchema(std::move(name), std::move(properties), std::move(required));
			tool["description"] = "Bounded UAM mutation. Requires the owning chat's current UAM YOLO permission mode and is forbidden in Plan mode; otherwise UAM rejects it without changing state.";
			return tool;
		}

		nlohmann::json Tools()
		{
			return nlohmann::json::array({
			    ToolSchema("skill_list"),
			    ToolSchema("skill_read", {{"id", {{"type", "string"}}}}, {"id"}),
			    ToolSchema("agent_list"),
			    ToolSchema("agent_status", {{"runId", {{"type", "string"}}}}, {"runId"}),
			    ToolSchema("goal_get"),
			    MutationToolSchema("agent_delegate", {{"agentId", {{"type", "string"}}}, {"task", {{"type", "string"}}},
			                                                 {"providerId", {{"type", "string"}}}, {"modelId", {{"type", "string"}}}},
			                               {"agentId", "task"}),
			    MutationToolSchema("agent_cancel", {{"runId", {{"type", "string"}}}}, {"runId"}),
			    MutationToolSchema("goal_create", {{"objective", {{"type", "string"}}}, {"idempotencyKey", {{"type", "string"}}}}, {"objective", "idempotencyKey"}),
			});
		}

		std::optional<nlohmann::json> SubmitToManager(const std::filesystem::path& capability,
		                                              std::string_view method,
		                                              const nlohmann::json& arguments)
		{
			if (!uam::paths::IsDirectoryNoThrow(capability) || uam::paths::IsLinkOrReparsePointNoThrow(capability))
				return std::nullopt;
			const std::string id = RandomRequestId();
			const nlohmann::json request{{"requestId", id}, {"method", method}, {"arguments", arguments}};
			const std::string request_text = request.dump();
			if (request_text.size() > kMaxRequestBytes ||
			    !uam::io::WriteTextFile(capability / "requests" / (id + ".json"), request_text)) return std::nullopt;
			const std::filesystem::path response_path = capability / "responses" / (id + ".json");
			for (int attempt = 0; attempt < 400; ++attempt)
			{
				const auto size = uam::paths::FileSizeNoThrow(response_path);
				if (size.has_value())
				{
					if (*size == 0 || *size > kMaxResponseBytes || uam::paths::IsLinkOrReparsePointNoThrow(response_path)) return std::nullopt;
					std::string text;
					if (!uam::io::TryReadTextFile(response_path, text) || text.size() != *size) return std::nullopt;
					uam::paths::RemoveFileNoThrow(response_path);
					const nlohmann::json response = nlohmann::json::parse(text, nullptr, false);
					return response.is_object() ? std::optional<nlohmann::json>{response} : std::nullopt;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}
			return std::nullopt;
		}
	} // namespace

	bool UamControlService::IsStdioServerInvocation(const std::vector<std::string>& arguments)
	{
		return std::ranges::find(arguments, kServerFlag) != arguments.end();
	}

	bool UamControlService::SupportsStructuredProtocol(std::string_view protocol)
	{
		return protocol == uam::provider_profile_constants::kProtocolGeminiAcp ||
		       protocol == uam::provider_profile_constants::kProtocolOpenCodeAcp ||
		       protocol == uam::provider_profile_constants::kProtocolCopilotAcp;
	}

		int UamControlService::RunStdioServerFromEnvironment()
	{
		const auto capability = uam::env::GetTrimmedPath(kCapabilityDirectoryEnvironment);
		if (!capability.has_value()) return 2;
		std::string line;
		while (std::getline(std::cin, line))
		{
			if (line.size() > kMaxRequestBytes) return 2;
			const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
			if (!request.is_object()) continue;
			if (!request.contains("id")) continue;
			const nlohmann::json id = request["id"];
			nlohmann::json response{{"jsonrpc", "2.0"}, {"id", id}};
			if (!request.contains("method") || !request["method"].is_string())
			{
				response["error"] = {{"code", -32600}, {"message", "Invalid request."}};
				std::cout << response.dump() << '\n' << std::flush;
				continue;
			}
			const std::string method = request["method"].get<std::string>();
			if (method == "initialize")
			{
				response["result"] = {{"protocolVersion", "2025-03-26"}, {"capabilities", {{"tools", nlohmann::json::object()}}},
				                      {"serverInfo", {{"name", "uam-control"}, {"version", "1"}}}};
			}
			else if (method == "tools/list")
			{
				response["result"] = {{"tools", Tools()}};
			}
			else if (method == "tools/call")
			{
				if (!ValidStdioToolCall(request))
				{
					response["error"] = {{"code", -32602}, {"message", "Invalid tool call parameters."}};
				}
				else
				{
					const auto manager_response = SubmitToManager(
					    *capability, request["params"]["name"].get<std::string>(), request["params"]["arguments"]);
					const bool ok = manager_response.has_value() && manager_response->value("ok", false);
					const nlohmann::json payload = ok ? (*manager_response)["result"] : nlohmann::json{{"error", manager_response.has_value() ? manager_response->value("error", "Control request failed.") : "Control manager is unavailable."}};
					response["result"] = {{"content", nlohmann::json::array({{{"type", "text"}, {"text", payload.dump()}}})}, {"isError", !ok}};
				}
			}
			else
			{
				response["error"] = {{"code", -32601}, {"message", "Method not found."}};
			}
			const std::string output = response.dump();
			if (output.size() > kMaxResponseBytes) return 2;
			std::cout << output << '\n' << std::flush;
		}
		return 0;
	}

	bool UamControlService::Initialize(AppState& app, std::string* error_out)
	{
		for (auto& session : app.acp_sessions)
			if (session != nullptr) session->uam_control_capability_id.clear();
		app.uam_control_capabilities.clear();
		app.uam_control_manager_id = NewUuid();
		const std::filesystem::path root = ControlRoot(app);
		if (app.uam_control_manager_id.empty() ||
		    !uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(root) ||
		    !uam::paths::CreateDirectoriesNoThrow(root / app.uam_control_manager_id))
		{
			if (error_out != nullptr) *error_out = "UAM control runtime directory could not be initialized.";
			return false;
		}
		std::error_code permissions_error;
		std::filesystem::permissions(root, std::filesystem::perms::owner_all,
		                             std::filesystem::perm_options::replace, permissions_error);
		std::filesystem::permissions(root / app.uam_control_manager_id, std::filesystem::perms::owner_all,
		                             std::filesystem::perm_options::replace, permissions_error);
		if (permissions_error)
		{
			if (error_out != nullptr) *error_out = "UAM control runtime directory could not be restricted to its owner.";
			Shutdown(app);
			return false;
		}
		return true;
	}

	void UamControlService::Shutdown(AppState& app)
	{
		for (auto& session : app.acp_sessions)
			if (session != nullptr) session->uam_control_capability_id.clear();
		app.uam_control_capabilities.clear();
		if (!app.data_root.empty() && !app.uam_control_manager_id.empty())
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(ControlRoot(app));
		app.uam_control_manager_id.clear();
	}

	bool UamControlService::AppendSessionMcpServer(AppState& app, AcpSessionState& session,
	                                               const ChatSession& chat, std::string_view setup_method,
	                                               nlohmann::json& request, std::string* error_out)
	{
		if (!chat.uam_control_enabled || session.model_discovery_only) return true;
		if (setup_method != uam::acp_methods::kSessionNew && setup_method != uam::acp_methods::kSessionLoad && setup_method != uam::acp_methods::kSessionResume)
		{
			if (error_out != nullptr) *error_out = "UAM control is available only during structured session creation, load, or resume.";
			return false;
		}
		if (!request.contains("params") || !request["params"].is_object() ||
		    !request["params"].contains("mcpServers") || !request["params"]["mcpServers"].is_array())
		{
			if (error_out != nullptr) *error_out = "Provider setup request cannot carry the UAM control MCP server.";
			return false;
		}
		for (const auto& server : request["params"]["mcpServers"])
			if (server.is_object() && server.value("name", "") == "uam-control")
			{
				if (error_out != nullptr) *error_out = "The MCP server name 'uam-control' is reserved by UAM.";
				return false;
			}

		RevokeForSession(app, session);
		const AgentRun* run = chat.agent_run_id.empty() ? nullptr : FindRun(app, chat.agent_run_id);
		const std::string root_chat_id = run == nullptr ? chat.id : run->root_chat_id;
		const ChatSession* root_chat = ChatDomainService().FindChatById(app, root_chat_id);
		if (root_chat == nullptr || !root_chat->uam_control_enabled)
		{
			if (error_out != nullptr) *error_out = "The owning root chat no longer permits UAM control.";
			return false;
		}
		UamControlCapability capability;
		capability.id = NewUuid();
		capability.directory = ControlRoot(app) / app.uam_control_manager_id / capability.id;
		capability.chat_id = root_chat_id;
		capability.session_chat_id = chat.id;
		capability.provider_id = session.provider_id;
		capability.agent_id = uam::strings::NonEmptyOrFallback(session.active_uam_agent_id, "build");
		capability.agent_run_id = run == nullptr ? std::string{} : run->id;
		capability.agent_skills = session.active_uam_agent_skills;
		capability.agent_delegates = session.active_uam_agent_delegates;
		capability.expires_at_epoch_ms = uam::time::SystemEpochMillisecondsNow() + kCapabilityLifetimeMs;
		if (capability.id.empty() || !uam::paths::CreateDirectoriesNoThrow(capability.directory / "requests") ||
		    !uam::paths::CreateDirectoriesNoThrow(capability.directory / "responses"))
		{
			if (error_out != nullptr) *error_out = "UAM control capability could not be created.";
			return false;
		}
		std::error_code permissions_error;
		std::filesystem::permissions(capability.directory, std::filesystem::perms::owner_all,
		                             std::filesystem::perm_options::replace, permissions_error);
		if (permissions_error)
		{
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
			if (error_out != nullptr) *error_out = "UAM control capability could not be restricted to its owner.";
			return false;
		}
		const std::filesystem::path executable = PlatformServicesFactory::Instance().process_service.ResolveCurrentExecutablePath();
		if (!uam::paths::IsRegularFileNoThrow(executable))
		{
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
			if (error_out != nullptr) *error_out = "UAM control executable is unavailable.";
			return false;
		}
		request["params"]["mcpServers"].push_back({
		    {"name", "uam-control"}, {"command", uam::paths::Utf8PathString(executable)},
		    {"args", nlohmann::json::array({std::string(kServerFlag)})},
		    {"env", nlohmann::json::array({{{"name", kCapabilityDirectoryEnvironment},
		                                      {"value", uam::paths::Utf8PathString(capability.directory)}}})},
		});
		session.uam_control_capability_id = capability.id;
		app.uam_control_capabilities.push_back(std::move(capability));
		return true;
	}

	void UamControlService::RevokeForSession(AppState& app, AcpSessionState& session)
	{
		if (session.uam_control_capability_id.empty()) return;
		const std::string id = std::move(session.uam_control_capability_id);
		session.uam_control_capability_id.clear();
		std::erase_if(app.uam_control_capabilities, [&](const UamControlCapability& capability) {
			if (capability.id != id) return false;
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
			return true;
		});
	}

	bool UamControlService::ProcessPendingRequests(AppState& app)
	{
		bool changed = false;
		std::size_t processed = 0;
		const int64_t now = uam::time::SystemEpochMillisecondsNow();
		for (std::size_t capability_index = 0;
		     capability_index < app.uam_control_capabilities.size() && processed < kMaxProcessedPerTick;)
		{
			UamControlCapability& capability = app.uam_control_capabilities[capability_index];
			if (!HasAuthority(app, capability, now))
			{
				(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
				for (auto& session : app.acp_sessions)
					if (session != nullptr && session->uam_control_capability_id == capability.id)
						session->uam_control_capability_id.clear();
				app.uam_control_capabilities.erase(app.uam_control_capabilities.begin() + static_cast<std::ptrdiff_t>(capability_index));
				changed = true;
				continue;
			}
			std::vector<std::filesystem::path> files;
			std::error_code list_error;
			bool queue_overflow = false;
			for (std::filesystem::directory_iterator it(capability.directory / "requests", list_error), end;
			     !list_error && it != end; it.increment(list_error))
			{
				const std::string name = uam::paths::Utf8PathString(it->path().filename());
				if (name.find(uam::io::kTempWritePathSuffix) != std::string::npos) continue;
				if (files.size() == kMaxQueuedRequests)
				{
					queue_overflow = true;
					break;
				}
				files.push_back(it->path());
			}
			if (list_error || queue_overflow)
			{
				(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
				for (auto& session : app.acp_sessions)
					if (session != nullptr && session->uam_control_capability_id == capability.id)
						session->uam_control_capability_id.clear();
				app.uam_control_capabilities.erase(app.uam_control_capabilities.begin() + static_cast<std::ptrdiff_t>(capability_index));
				changed = true;
				continue;
			}
			std::ranges::sort(files);
			const std::string capability_id = capability.id;
			const std::filesystem::path capability_directory = capability.directory;
			for (const auto& file : files)
			{
				if (processed == kMaxProcessedPerTick) break;
				++processed;
				const std::string request_id = uam::paths::Utf8PathString(file.stem());
				nlohmann::json response{{"ok", false}, {"error", "Malformed or oversized control request."}};
				const auto size = uam::paths::FileSizeNoThrow(file);
				if (SafeRequestId(request_id) && size.has_value() && *size > 0 && *size <= kMaxRequestBytes &&
				    file.extension() == ".json" && !uam::paths::IsLinkOrReparsePointNoThrow(file))
				{
					std::string text;
					if (uam::io::TryReadTextFile(file, text) && text.size() == *size &&
					    uam::paths::FileSizeNoThrow(file) == size &&
					    !uam::paths::IsLinkOrReparsePointNoThrow(file))
					{
						const nlohmann::json request = nlohmann::json::parse(text, nullptr, false);
						response = HandleRequest(app, capability_id, request, now);
					}
				}
				(void)uam::paths::RemoveFileNoThrow(file);
				const std::string response_text = response.dump();
				if (response_text.size() <= kMaxResponseBytes)
					(void)uam::io::WriteTextFile(capability_directory / "responses" / (request_id + ".json"), response_text);
				changed = true;
			}
			++capability_index;
		}
		return changed;
	}

	nlohmann::json UamControlService::HandleRequestForTests(AppState& app,
	                                                        const std::string& capability_id,
	                                                        const nlohmann::json& request,
	                                                        int64_t now_epoch_ms)
	{
		return HandleRequest(app, capability_id, request, now_epoch_ms);
	}

	bool UamControlService::ValidStdioToolCallForTests(const nlohmann::json& request)
	{
		return ValidStdioToolCall(request);
	}
} // namespace uam
