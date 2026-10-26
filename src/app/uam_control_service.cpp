#include "app/managed_parent_continuation_service.h"
#include "common/config/provider_availability.h"
#include "common/provider/provider_setup.h"
#include <condition_variable>
#include <mutex>
#include "app/uam_control_service.h"
#include "app/data_folder_transition.h"
#include "common/platform/data_folder_writer_lease.h"

#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/provider_model_catalog_service.h"
#include "common/config/execution_host_config.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "app/chat_domain_service.h"
#include "app/computer_use_service.h"
#include "app/goal_service.h"
#include "app/markdown_store_service.h"
#include "app/managed_subagent_control_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/central_provider_configuration.h"
#include "common/config/settings_normalization.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_profile_constants.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_control_capability_retention.h"
#include "common/runtime/app_time.h"
#include "common/security/command_safety.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <deque>
#include <functional>
#include <future>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <thread>

namespace uam
{
	struct UamWaitResponsePublication
	{
		io::AtomicWriteResult write;
		bool rename_crossed = false;
		bool outcome_unknown = false;
	};

	// Opaque shared ownership lets capability copies/retirement retain real IO futures safely.
	struct UamAgentWait
	{
		using Clock = std::chrono::steady_clock;
		struct RunIdentity
		{
			std::string id, root_id, parent_id, transcript_id, provider_id, model_id, definition_hash, created_at;
			std::string agent_id, execution_capability, resumed_from_id, result_delivery_id, workspace_access, definition_snapshot;
			std::shared_ptr<const std::uint8_t> transcript_incarnation;
			std::string host_id; // Execution host is birth scope; native identity/model/worktree publication is late-bound.
		};
		enum class Phase { Waiting, AuditPending, ResponsePending, Published, Retired };
		Phase phase = Phase::Waiting;
		std::filesystem::path data_root, directory;
		std::string capability_id, request_id, run_id, root_id, session_chat_id, provider_id, connection_id;
		std::shared_ptr<const std::uint8_t> root_incarnation, chat_incarnation;
		std::shared_ptr<const char> launch_incarnation;
		const AcpSessionState* original_session = nullptr; // Compared only on the owner; never dereferenced by a worker.
		std::string session_id, native_session_id, process_id, process_host_id, remote_delivery_token, native_turn_id;
		int turn_serial = 0;
		ExecutionHost host;
		std::string root_workspace, root_worktree, workspace, worktree;
		std::string root_provider_id, root_model_id, root_native_id, chat_provider_id, chat_model_id, origin_agent_run_id;
		std::vector<RunIdentity> ancestry;
		Clock::time_point semantic_deadline, publication_deadline, retry_at;
		std::string sealed_response;
		UamControlAuditRecord audit;
		std::shared_ptr<ControlAuditSaveTicket> audit_ticket;
		ControlAuditSaveResult audit_result;
		std::shared_ptr<std::atomic<bool>> publish_lease = std::make_shared<std::atomic<bool>>(true);
		std::future<UamWaitResponsePublication> response_future;
		UamWaitResponsePublication response_result;
		bool response_observed = false;
	};

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
		constexpr std::size_t kMaxActiveWaits = 16;
		constexpr std::size_t kMaxWaitActionsPerTick = 2;
		constexpr std::size_t kMaxCancelledWaitsPerConnection = 8;
		constexpr auto kAgentWaitSlice = std::chrono::seconds(80);
		constexpr auto kAgentWaitPublicationBudget = std::chrono::seconds(100);
		constexpr int64_t kCapabilityLifetimeMs = 24LL * 60LL * 60LL * 1000LL;
		constexpr int64_t kApprovalLifetimeMs = 90LL * 1000LL;
		constexpr int64_t kModelGoalTokenBudget = 200000;
		constexpr std::string_view kServerFlag = "--uam-control-mcp";
		constexpr const char* kCapabilityDirectoryEnvironment = "UAM_CONTROL_CAPABILITY_DIR";

		enum class WaitAbortReason { None, Cancelled, InputClosed, InputError, Deadline };

		/// Sole stdin consumer; no blocked reader thread can survive an error/early return.
		class StdioInputPump
		{
		  public:
			explicit StdioInputPump(IPlatformProcessService& service) : m_service(service) {}

			bool Next(std::string& line)
			{
				while (m_lines.empty() && !m_eof && !m_failed)
				{
					Pump();
					if (m_lines.empty() && !m_eof && !m_failed)
						std::this_thread::sleep_for(std::chrono::milliseconds(25));
				}
				if (m_failed || m_lines.empty()) return false;
				line = std::move(m_lines.front());
				m_lines.pop_front();
				return true;
			}

			void BeginWait(const nlohmann::json& id)
			{
				m_wait_id = id;
				m_waiting = true;
				m_cancelled = false;
				// A request and its cancellation may already share the same input chunk.
				for (auto line = m_lines.begin(); line != m_lines.end();)
				{
					if (IsMatchingCancellation(*line))
					{
						m_cancelled = true;
						line = m_lines.erase(line);
					}
					else ++line;
				}
			}

			WaitAbortReason ProbeWait()
			{
				Pump();
				if (m_failed) return WaitAbortReason::InputError;
				if (m_eof) return WaitAbortReason::InputClosed;
				return m_cancelled ? WaitAbortReason::Cancelled : WaitAbortReason::None;
			}

			void EndWait() { m_waiting = false; m_wait_id = nullptr; m_cancelled = false; }
			bool Failed() const { return m_failed; }

		  private:
			bool IsMatchingCancellation(std::string_view line) const
			{
				if (!m_waiting) return false;
				const nlohmann::json message = nlohmann::json::parse(line, nullptr, false);
				if (!message.is_object() || message.contains("id") ||
				    message.value("method", nlohmann::json{}) != "notifications/cancelled" ||
				    !message.contains("params") || !message["params"].is_object()) return false;
				const auto id = message["params"].find("requestId");
				return id != message["params"].end() &&
				       (id->is_string() || id->is_number_integer()) && *id == m_wait_id;
			}

			void CompleteLine()
			{
				if (IsMatchingCancellation(m_partial)) m_cancelled = true;
				else
				{
					if (m_lines.size() >= kMaxQueuedRequests) { m_failed = true; return; }
					m_lines.push_back(std::move(m_partial));
				}
				m_partial.clear();
			}

			void Pump()
			{
				using platform::StandardInputReadStatus;
				// Bound work per mailbox tick as well as total queued input memory.
				for (int chunk = 0; chunk < 16 && !m_eof && !m_failed; ++chunk)
				{
					std::array<char, 4096> bytes{};
					const auto read = m_service.ReadStandardInput(bytes.data(), bytes.size());
					if (read.status == StandardInputReadStatus::Pending) return;
					if (read.status == StandardInputReadStatus::EndOfStream)
					{
						m_eof = true;
						if (!m_partial.empty()) CompleteLine(); // Preserve final non-newline line.
						return;
					}
					if (read.status != StandardInputReadStatus::Data || read.size == 0 || read.size > bytes.size())
					{
						m_failed = true;
						return;
					}
					for (std::size_t i = 0; i < read.size && !m_failed; ++i)
					{
						if (bytes[i] == '\n') CompleteLine();
						else if (m_partial.size() >= kMaxRequestBytes) m_failed = true;
						else m_partial.push_back(bytes[i]);
					}
				}
			}

			IPlatformProcessService& m_service;
			std::deque<std::string> m_lines;
			std::string m_partial;
			nlohmann::json m_wait_id;
			bool m_waiting = false;
			bool m_cancelled = false;
			bool m_eof = false;
			bool m_failed = false;
		};

		struct ToolResult
		{
			bool ok = false;
			bool deferred = false;
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
			if (method == "_mcp_cancel_request")
				return HasOnlyStringArguments(arguments, {"connectionId", "requestId"}) &&
				    SafeRequestId(arguments["connectionId"].get_ref<const std::string&>()) &&
				    SafeRequestId(arguments["requestId"].get_ref<const std::string&>());
			if (method == "_mcp_initialized" || method == "_mcp_disconnected" || method == "_mcp_heartbeat")
				return HasOnlyStringArguments(arguments, {"connectionId"}) && SafeRequestId(arguments["connectionId"].get_ref<const std::string&>());
			if (method == "skill_list" || method == "agent_list" || method == "goal_get")
				return arguments.is_object() && arguments.empty();
			if (method == "skill_read") return HasOnlyStringArguments(arguments, {"id"}, {"path"});
			if (method == "agent_status" || method == "agent_cancel" || method == "agent_wait")
				return HasOnlyStringArguments(arguments, {"runId"});
			if (method == "agent_delegate")
				return HasOnlyStringArguments(arguments, {"agentId", "task"}, {"providerId", "modelId"});
			if (method == "goal_create")
				return HasOnlyStringArguments(arguments, {"objective", "idempotencyKey"});
			if (method == "computer_use_request")
				return HasOnlyStringArguments(arguments, {"reason"});
			if (method == "user_question")
			{
				if (!arguments.is_object() || !arguments.contains("question") ||
				    !arguments["question"].is_string() || arguments.size() > 2)
					return false;
				const std::string question = arguments["question"].get<std::string>();
				if (question.empty() || question.size() > 2048 || question.find('\0') != std::string::npos)
					return false;
				if (!arguments.contains("options")) return arguments.size() == 1;
				const nlohmann::json& options = arguments["options"];
				if (!options.is_array() || options.size() > 3) return false;
				std::set<std::string> seen;
				for (const nlohmann::json& option : options)
				{
					if (!option.is_string()) return false;
					const std::string label = option.get<std::string>();
					if (label.empty() || !SafeText(label, 128) || !seen.insert(label).second) return false;
				}
				return true;
			}
			return false;
		}

		bool ValidStdioToolCall(const nlohmann::json& request)
		{
			return request.is_object() && request.contains("method") && request["method"].is_string() &&
			       request["method"] == "tools/call" && request.contains("params") && request["params"].is_object() &&
			       request["params"].contains("name") && request["params"]["name"].is_string() &&
			       request["params"].contains("arguments") && request["params"]["arguments"].is_object() &&
			       !request["params"]["name"].get_ref<const std::string&>().starts_with("_mcp_");
		}

		bool Contains(const std::vector<std::string>& values, std::string_view value)
		{
			return std::ranges::find(values, value) != values.end();
		}

		bool IsMutation(std::string_view method)
		{
			return method == "agent_delegate" || method == "agent_cancel" || method == "goal_create";
		}

		bool ReadOnlyAgent(const AppState& app, const UamControlCapability& capability)
		{
			if (capability.agent_id == "plan") return true;
			for (const auto& session : app.acp_sessions)
				if (session != nullptr && session->uam_control_capability_id == capability.id)
					return session->active_uam_agent_workspace_access == "read";
			for (const auto& terminal : app.cli_terminals)
				if (terminal != nullptr && terminal->uam_control_session != nullptr && terminal->uam_control_session->uam_control_capability_id == capability.id)
					return terminal->uam_control_session->active_uam_agent_workspace_access == "read";
			return false;
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

		/// A provider override grants this launch; later settings changes do not alter its authority.
		bool ControlEnabled(const ChatSession* chat, const UamControlCapability& capability, bool root)
		{
			return chat != nullptr && !chat->uam_control_explicitly_disabled && (chat->uam_control_enabled ||
			    (root ? capability.root_control_override_at_launch : capability.session_control_override_at_launch));
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
			    {"lastBlocker", goal->last_blocker}, {"lastBlockerKind", goal->last_blocker_kind},
			    {"lastDiagnostic", goal->last_diagnostic},
			    {"lastVerification", goal->last_verification}, {"currentStep", goal->current_step},
			    {"completedItems", goal->completed_items}, {"remainingItems", goal->remaining_items},
			    {"maxLoopIterations", bounded_settings.goal_max_loop_iterations},
			    {"executionOwner", goal->execution_owner == "provider" ? "provider" : "uam"}, {"creator", goal->creator},
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

		bool WriteResponse(const std::filesystem::path& directory, std::string_view request_id,
		                   const nlohmann::json& response)
		{
			if (!SafeRequestId(request_id)) return false;
			const std::string text = response.dump();
			if (text.size() > kMaxResponseBytes) return false;
			const std::filesystem::path path = directory / "responses" /
			                                   (std::string(request_id) + ".json");
			return !uam::paths::IsLinkOrReparsePointNoThrow(path) &&
			       uam::io::WriteTextFile(path, text);
		}

		std::string ApprovalRequestId(const UamControlCapability& capability)
		{
			return "uam-control:" + capability.id + ":" + capability.pending_approval->request_id;
		}

		std::string_view ApprovalChatId(const UamControlCapability& capability)
		{
			return capability.pending_approval->method == "computer_use_request" ||
			               capability.pending_approval->method == "user_question"
			           ? std::string_view(capability.session_chat_id)
			           : std::string_view(capability.chat_id);
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

		nlohmann::json CentralSkillFiles(const UamControlCapability& capability)
		{
			if (capability.provider_configuration_directory.empty()) return nlohmann::json::object();
			std::string bytes;
			std::string error;
			if (!uam::provider_setup::ReadFile(capability.provider_configuration_directory / "owner.json", bytes, error)) return nlohmann::json::object();
			const nlohmann::json manifest = nlohmann::json::parse(bytes, nullptr, false);
			if (!uam::provider_setup::ValidateManifest(manifest, error)) return nlohmann::json::object();
			return manifest["files"];
		}

		ToolResult SkillList(const AppState& app, const UamControlCapability& capability,
		                     const ChatSession& chat)
		{
			ToolResult out{.ok = true, .result = nlohmann::json::array(), .reason = "Listed approved skill metadata."};
			const std::vector<std::string> allowed = AllowedSkillIds(capability);
			const nlohmann::json files = CentralSkillFiles(capability);
			for (const auto& [name, entry] : files.items())
			{
				if (!name.starts_with("skills/") || !name.ends_with("/SKILL.md")) continue;
				const std::string id = name.substr(7, name.size() - 7 - 9);
				if (id.find('/') != std::string::npos || (!capability.central_skills_enabled && !Contains(allowed, id))) continue;
				out.result.push_back({{"id", id}, {"title", id}, {"relativePath", name}, {"version", "fnv1a64:" + entry["digest"].get<std::string>()}});
				if (out.result.size() == 64) break;
			}
			if (!out.result.empty() || allowed.empty()) return out;
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
			if (!capability.central_skills_enabled && !Contains(allowed, id)) return {.error = "Skill is not approved for this agent."};
			const std::string relative = arguments.value("path", "SKILL.md");
			const std::string name = "skills/" + id + "/" + relative;
			if (id.find_first_of("/\\") != std::string::npos || !uam::provider_setup::SafeRelativePath(name)) return {.error = "Skill asset path is invalid."};
			const nlohmann::json files = CentralSkillFiles(capability);
			if (files.contains(name))
			{
				std::string bytes;
				std::string read_error;
				if (!uam::provider_setup::ReadFile(capability.provider_configuration_directory / uam::paths::PathFromUtf8(name), bytes, read_error)) return {.error = read_error};
				if (bytes.size() > 32 * 1024 || uam::provider_setup::Digest(bytes) != files[name]["digest"].get<std::string>()) return {.error = "Skill asset is too large or differs from the installed snapshot."};
				ToolResult result{.ok = true, .result = {{"id", id}, {"path", relative}, {"version", "fnv1a64:" + files[name]["digest"].get<std::string>()}}, .reason = "Read installed skill asset."};
				if (bytes.find('\0') == std::string::npos) result.result["body"] = bytes;
				else result.result["bodyBase64"] = uam::base64::Encode(bytes);
				return result;
			}
			if (relative != "SKILL.md") return {.error = "Skill asset is not installed."};
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

		/// <summary>Lists bounded worker targets without probing providers or asserting authentication.</summary>
		nlohmann::json AgentProviderTargets(const AppState& app, const ChatSession& chat)
		{
			nlohmann::json targets = nlohmann::json::array();
			const std::string host_id = strings::NonEmptyOrFallback(chat.execution_host_id, "local");
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, host_id);
			const std::string workspace = paths::ResolveWorkspaceRootPath(app, chat).generic_string();
			const std::string parent_provider = provider_ids::NormalizeCliProviderAliasOrSelf(chat.provider_id);
			std::set<std::string> seen;
			for (const ProviderProfile& profile : app.provider_profiles)
			{
				const std::string id = provider_ids::NormalizeCliProviderAliasOrSelf(profile.id);
				// Managed children use structured chat with its native permission boundary;
				// SupportsTextWorkers describes the separate tool-free worker launcher.
				if (!provider_ids::IsKnownCliProviderId(id) || !profile.supports_cli || !profile.supports_structured || !ProviderRuntimeRegistry::ResolveById(id).IsEnabled() || !seen.insert(id).second) continue;
				std::string reason;
				const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find(CliProviderVersionStateKey(id, host_id));
				if (host == nullptr) reason = "Execution host unavailable.";
				else if (host->transport == "ssh" && host->runner_status != "ready") reason = "Execution host is not ready.";
				else if (host->transport == "ssh" && !execution_hosts::IsAbsoluteRemotePath(host->platform, workspace)) reason = "Remote workspace unavailable.";
				else if (!provider_availability::IsEnabled(app.settings, id, host_id)) reason = provider_availability::DisabledReason();
				else if (version == app.runtime_cli_versions_by_provider_id.end() || !version->second.checked) reason = "CLI compatibility has not been checked.";
				else if (!execution_hosts::SameConnection(*host, version->second.execution_host)) reason = "CLI observation belongs to a different host configuration.";
				else if (!version->second.supported || version->second.installed_version.empty() || !version->second.check_error.empty()) reason = "CLI is unavailable or incompatible.";
				else reason = ProviderCliLaunchBlockReason(app, id, host_id);
				nlohmann::json choices = app.provider_model_catalog != nullptr
				    ? app.provider_model_catalog->GetCachedProviderModelChoices(id, workspace, host_id) : nlohmann::json::array();
				const std::string default_model = id == parent_provider ? chat.model_id : std::string{};
				targets.push_back({{"providerId", id}, {"title", profile.title}, {"executionHostId", host_id},
				    {"eligible", reason.empty()}, {"availability", reason.empty() ? "installed-compatible" : "unavailable"},
				    {"unavailableReason", reason}, {"authentication", "unverified"},
				    {"defaultModelId", default_model}, {"models", std::move(choices)}});
				if (targets.size() == 8) break;
			}
			return targets;
		}

		ToolResult AgentList(const AppState& app, const UamControlCapability& capability,
		                     const ChatSession& chat)
		{
			ToolResult out{.ok = true, .result = nlohmann::json::array(), .reason = "Listed bounded delegate metadata."};
			const AgentDefinitionCatalog catalog = AgentCatalog(app, chat);
			const nlohmann::json targets = AgentProviderTargets(app, chat);
			for (const AgentDefinition& definition : catalog.definitions)
			{
				if (definition.mode != "subagent" && definition.mode != "both") continue;
				if (!capability.agent_run_id.empty() && !Contains(capability.agent_delegates, definition.id)) continue;
				const bool forced_read = definition.workspace_access == "read" ||
				                         (!capability.agent_run_id.empty() && FindRun(app, capability.agent_run_id) != nullptr &&
				                          FindRun(app, capability.agent_run_id)->effective_workspace_access == "read");
				out.result.push_back({{"id", definition.id}, {"description", definition.description},
				                      {"workspaceAccess", forced_read ? "read" : "write"},
				                      {"providerNeutral", true}, {"providerTargets", targets}});
				if (out.result.dump().size() > kMaxResponseBytes - 1024)
				{
					out.result.erase(out.result.end() - 1);
					break;
				}
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
			                                agent_id, task, &run_id, &error, provider_id, model_id, ReadOnlyAgent(app, capability)))
				return {.error = std::move(error)};
			capability.owned_agent_run_ids.insert(run_id);
			const auto child = std::ranges::find(app.agent_runs, run_id, &AgentRun::id);
			const std::string delivery = child == app.agent_runs.end() ? "cooperative-only" :
			    ManagedParentContinuationService::StageDelegation(app, capability, *child);
			return {.ok = true, .result = {{"runId", run_id}, {"status", "queued"}, {"resultDelivery", delivery}, {"resultDeliveryReason", delivery == "automatic-pending" ? "same_live_local_codex" : delivery == "held" ? "continuation_held" : "adapter_or_scope_unavailable"}},
			        .reason = "Queued one bounded child run."};
		}

		nlohmann::json AgentStatusPayload(const AgentRun& run)
		{
			return {{"runId", run.id}, {"status", run.status}, {"agentId", run.agent_id},
			                   {"providerId", run.provider_id}, {"modelId", run.model_id},
			                   {"executionCapability", run.execution_capability},
			                   {"resumedFromRunId", run.resumed_from_run_id},
			                   {"definitionHash", run.definition_hash},
			                   {"transcriptChatId", run.transcript_chat_id},
			                   {"result", run.result_excerpt}, {"diagnosticCode", run.diagnostic_code},
			                   {"diagnostic", run.diagnostic}};
		}

		ToolResult AgentStatus(const AppState& app, const UamControlCapability& capability,
		                       const nlohmann::json& arguments)
		{
			const std::string run_id = arguments.value("runId", "");
			const AgentRun* run = FindRun(app, run_id);
			if (run == nullptr || !IsOwnedDescendant(app, capability, *run))
				return {.error = "Agent run is outside this capability's descendant scope."};
			return {.ok = true,
			        .result = AgentStatusPayload(*run),
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
			const bool mutations_allowed = chat != nullptr && !ReadOnlyAgent(app, capability) && MutationsAllowed(*chat);
			const bool goal_creation_allowed = chat != nullptr && !ReadOnlyAgent(app, capability) &&
			    uam::approval_modes::AppApprovalModeOrEmpty(chat->approval_mode) !=
			        uam::approval_modes::kPlanApprovalMode;
			return {.ok = true,
			        .result = {{"goal", GoalJson(GoalService::FindActiveOrLatestTerminalGoal(app, capability.chat_id), app)},
			                   {"controlEnabled", ControlEnabled(chat, capability, true)},
			                   {"mutationToolsEnabled", mutations_allowed},
			                   {"mutationPolicy", mutations_allowed ? "yoloAutoApprove" : "readOnly"},
			                   {"delegationPolicy", "inheritParentPermissions"},
			                   {"agentGoalManagementEnabled", goal_creation_allowed}, {"lifecycleOwner", "uam"},
			                   {"modelMayCreateOnly", goal_creation_allowed},
			                   {"goalCreationPolicy", mutations_allowed ? "yoloAutoApprove" :
			                       goal_creation_allowed ? "userApproval" : "disabled"}},
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
			ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (chat == nullptr || std::ranges::any_of(chat->goals, [&](const Goal& goal) {
				    return goal.creator == "model" && goal.creator_request_key_hash == idempotency_hash;
			    }))
				return {.error = "Goal creation idempotency key was already used."};
			if (GoalService::FindActiveGoal(app, capability.chat_id) != nullptr)
				return {.error = "This chat already has an active goal."};
			const ChatSession before = *chat;
			std::string goal_id;
			if (!GoalService::CreateGoal(app, capability.chat_id, objective, kModelGoalTokenBudget,
			                             &goal_id, "uam", "", "model", capability.provider_id,
			                             capability.agent_id, capability.agent_run_id, idempotency_hash) ||
			    !GoalService::SetActiveGoal(app, capability.chat_id, goal_id))
			{
				*chat = before;
				return {.error = "UAM could not create and activate the goal."};
			}
			return {.ok = true, .result = {{"goalId", goal_id}, {"status", "active"},
			                              {"tokenBudget", kModelGoalTokenBudget}, {"lifecycleOwner", "uam"}},
			        .reason = "Created one visible bounded UAM-owned goal."};
		}

		ToolResult QueueApproval(AppState& app, UamControlCapability& capability,
		                         UamControlCapability::PendingApproval pending)
		{
			const auto session = std::ranges::find_if(app.acp_sessions, [&](const auto& value) {
				return value != nullptr && value->chat_id == capability.session_chat_id &&
				       value->uam_control_capability_id == capability.id;
			});
			if (session != app.acp_sessions.end() &&
			    (!(*session)->pending_user_input.request_id_json.empty() ||
			     !(*session)->pending_permission.request_id_json.empty()))
				return {.error = "Finish the current provider question or permission request first."};
			const std::string_view target_chat_id = pending.method == "goal_create"
			    ? std::string_view(capability.chat_id) : std::string_view(capability.session_chat_id);
			if (std::ranges::any_of(app.uam_control_capabilities, [&](const UamControlCapability& value) {
				    return value.pending_approval.has_value() && ApprovalChatId(value) == target_chat_id;
			    }))
				return {.error = "Another UAM request is already waiting for approval in this chat."};
			capability.pending_approval = std::move(pending);
			if (ChatSession* chat = ChatDomainService().FindChatById(app, std::string(target_chat_id)))
			{
				chat->interaction_at = uam::time::InteractionTimestampNow();
				if (!app.data_root.empty() && !ChatRepository::SaveLastOpenedAt(app.data_root, *chat))
					app.pending_chat_save_at_by_chat_id[chat->id] = 0.0;
			}
			return {.deferred = true};
		}

		ToolResult GoalCreateRequest(AppState& app, UamControlCapability& capability,
		                             std::string_view request_id, const nlohmann::json& arguments,
		                             int64_t now_epoch_ms)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (!ControlEnabled(chat, capability, true))
				return {.error = "UAM Control is disabled or the owning chat is unavailable."};
			if (uam::approval_modes::AppApprovalModeOrEmpty(chat->approval_mode) ==
			    uam::approval_modes::kPlanApprovalMode)
				return {.error = "Goals cannot be created in Plan mode."};
			const std::string objective = uam::strings::Trim(arguments.value("objective", ""));
			const std::string key = uam::strings::Trim(arguments.value("idempotencyKey", ""));
			if (objective.empty() || !SafeText(objective, 2048))
				return {.error = "Goals requiring GUI approval must have an objective of at most 2 KiB."};
			if (!SafeRequestId(key))
				return {.error = "Goal creation requires a bounded idempotency key."};
			const std::string key_hash = "fnv1a64:" + uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(key));
			if (std::ranges::any_of(chat->goals, [&](const Goal& goal) {
			    return goal.creator == "model" && goal.creator_request_key_hash == key_hash;
			})) return {.error = "Goal creation idempotency key was already used."};
			if (GoalService::FindActiveGoal(app, capability.chat_id) != nullptr)
				return {.error = "This chat already has an active goal."};
			return QueueApproval(app, capability, {.request_id = std::string(request_id),
			                                     .method = "goal_create", .objective = objective,
			                                     .idempotency_key = key,
			                                     .expires_at_epoch_ms = now_epoch_ms + kApprovalLifetimeMs});
		}

		/// Capture only the original user message owned by this active structured turn.
		/// A queued message or the model's permission reason cannot authorise application access.
		bool CaptureComputerUseOrigin(const AppState& app, const UamControlCapability& capability,
		                             std::string& task, std::string& origin)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
			const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
			const AcpSessionState* session = FindAcpSessionForChat(app, capability.session_chat_id);
			if (!ControlEnabled(chat, capability, false) || !ControlEnabled(root, capability, true) ||
			    !app.settings.computer_use_globally_enabled || !uam::computer_use::AvailableForChat(*chat) ||
			    !uam::computer_use::UsesUamBackend(*chat) || ReadOnlyAgent(app, capability) ||
			    uam::approval_modes::AppApprovalModeOrEmpty(chat->approval_mode) == uam::approval_modes::kPlanApprovalMode ||
			    uam::approval_modes::AppApprovalModeOrEmpty(root->approval_mode) == uam::approval_modes::kPlanApprovalMode ||
			    session == nullptr || !session->running || !session->processing || session->cancel_requested ||
			    session->uam_control_capability_id != capability.id || session->provider_id != capability.provider_id ||
			    provider_ids::NormalizeCliProviderAliasOrSelf(chat->provider_id) != provider_ids::NormalizeCliProviderAliasOrSelf(session->provider_id) ||
			    session->turn_serial <= 0 || session->turn_first_user_message_index < 0 ||
			    session->turn_user_message_index < session->turn_first_user_message_index ||
			    session->turn_user_message_index >= static_cast<int>(chat->messages.size())) return false;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id);
			if (host == nullptr || host->transport != "local") return false;
			const Message& user = chat->messages[static_cast<std::size_t>(session->turn_first_user_message_index)];
			if (user.role != MessageRole::User || user.acp_prompt_not_sent || user.created_at.empty() ||
			    user.content.empty() || !SafeText(user.content, 1024U * 1024U)) return false;
			task = user.content;
			origin = nlohmann::json::array({app.data_root.generic_string(), chat->id, chat->created_at,
			    root->id, root->created_at, root->provider_id, root->execution_host_id, root->workspace_directory,
			    root->approval_mode, root->uam_control_enabled, root->uam_control_explicitly_disabled,
			    chat->provider_id, chat->execution_host_id, chat->workspace_directory, chat->native_session_id,
			    chat->approval_mode, chat->computer_use_backend, chat->computer_use_enabled,
			    session->session_id, session->last_process_id, session->process_execution_host_id,
			    session->turn_serial, session->turn_first_user_message_index, session->turn_user_message_index,
			    session->active_uam_agent_id, session->active_uam_agent_definition_hash, session->active_uam_agent_workspace_access,
			    user.created_at, user.content, host->id, host->transport, host->ssh_alias, host->platform,
			    host->architecture, host->runner_version, host->runner_protocol_version, host->runner_directory,
			    host->instruction_file, host->instruction_text ? nlohmann::json(*host->instruction_text) : nlohmann::json(nullptr)}).dump();
			return true;
		}

		bool ComputerUseApprovalCurrent(const AppState& app, const UamControlCapability& capability)
		{
			if (!capability.pending_approval || capability.pending_approval->method != "computer_use_request") return true;
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
			const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
			const auto& pending = *capability.pending_approval;
			std::string task, origin;
			return chat != nullptr && root != nullptr && pending.computer_use_chat_incarnation == chat->metadata_incarnation &&
			    pending.computer_use_root_incarnation == root->metadata_incarnation &&
			    CaptureComputerUseOrigin(app, capability, task, origin) &&
			    task == pending.computer_use_task && origin == pending.computer_use_origin;
		}

		ToolResult ComputerUseRequest(AppState& app, UamControlCapability& capability,
		                              std::string_view request_id, const nlohmann::json& arguments,
		                              int64_t now_epoch_ms)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
			const ChatSession* root_chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (!ControlEnabled(chat, capability, false) || !ControlEnabled(root_chat, capability, true))
				return {.error = "UAM Control is disabled or the owning chat is unavailable."};
			if (!app.settings.computer_use_globally_enabled)
				return {.error = "Computer Use is disabled globally."};
			if (!uam::computer_use::AvailableForChat(*chat) || !uam::computer_use::UsesUamBackend(*chat))
				return {.error = "UAM Computer Use is unavailable for this chat or execution host."};
			if (uam::approval_modes::AppApprovalModeOrEmpty(chat->approval_mode) ==
			    uam::approval_modes::kPlanApprovalMode)
				return {.error = "Computer Use cannot be enabled in Plan mode."};
			const std::string reason = uam::strings::SafeLine(arguments.value("reason", ""), 513, true);
			if (reason.empty() || !SafeText(reason, 512))
				return {.error = "Computer Use requests need a reason of at most 512 bytes."};
			if (chat->computer_use_enabled)
				return {.ok = true, .result = {{"enabled", true}, {"chatId", chat->id}},
				        .reason = "Computer Use was already enabled for this chat."};
			std::string task, origin;
			if (!CaptureComputerUseOrigin(app, capability, task, origin))
				return {.error = "The original owned computer-use user turn is unavailable."};
			UamControlCapability::PendingApproval pending;
			pending.request_id = std::string(request_id);
			pending.method = "computer_use_request";
			pending.reason = reason;
			pending.computer_use_task = std::move(task);
			pending.computer_use_origin = std::move(origin);
			pending.computer_use_chat_incarnation = chat->metadata_incarnation;
			pending.computer_use_root_incarnation = root_chat->metadata_incarnation;
			pending.expires_at_epoch_ms = now_epoch_ms + kApprovalLifetimeMs;
			return QueueApproval(app, capability, std::move(pending));
		}

		ToolResult UserQuestionRequest(AppState& app, UamControlCapability& capability,
		                              std::string_view request_id, const nlohmann::json& arguments,
		                              int64_t now_epoch_ms)
		{
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
			const ChatSession* root_chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (!ControlEnabled(chat, capability, false) || !ControlEnabled(root_chat, capability, true))
				return {.error = "UAM Control is disabled or the owning chat is unavailable."};
			std::vector<std::string> options;
			for (const nlohmann::json& option : arguments.value("options", nlohmann::json::array()))
				options.push_back(option.get<std::string>());
			return QueueApproval(app, capability, {.request_id = std::string(request_id),
			                                     .method = "user_question",
			                                     .question = arguments.at("question").get<std::string>(),
			                                     .question_options = std::move(options),
			                                     .expires_at_epoch_ms = now_epoch_ms + kApprovalLifetimeMs});
		}

		ToolResult ExecuteTool(AppState& app, UamControlCapability& capability,
		                       std::string_view method, const nlohmann::json& arguments)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (!ControlEnabled(chat, capability, true))
				return {.error = "Agent-goal management is disabled or the owning chat is unavailable."};
			if (method != "agent_delegate" && IsMutation(method) && !MutationsAllowed(*chat))
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
			if (session != app.acp_sessions.end()) return true;
			return std::ranges::any_of(app.cli_terminals, [&](const auto& terminal) {
				return terminal != nullptr && terminal->uam_control_session != nullptr &&
				    terminal->uam_control_session->chat_id == capability.session_chat_id &&
				    terminal->uam_control_session->uam_control_capability_id == capability.id &&
				    terminal->uam_control_session->provider_id == capability.provider_id &&
				    (terminal->running || terminal->context_preparation != nullptr);
			});
		}

		const AcpSessionState* WaitSession(const AppState& app, const UamControlCapability& capability)
		{
			for (const auto& session : app.acp_sessions)
				if (session && session->chat_id == capability.session_chat_id && session->uam_control_capability_id == capability.id &&
				    session->provider_id == capability.provider_id) return session.get();
			// CLI View has no proven original native turn here; its other tools are unchanged.
			return nullptr;
		}

		bool IsWaitOwnedDescendant(const AppState& app, const UamControlCapability& capability, const AgentRun& candidate)
		{
			std::array<std::string_view, 3> visited{};
			bool owned = false;
			const AgentRun* run = &candidate;
			for (std::size_t depth = 0; depth < visited.size(); ++depth)
			{
				if (run->root_chat_id != capability.chat_id || run->id.empty() ||
				    std::find(visited.begin(), visited.begin() + static_cast<std::ptrdiff_t>(depth), std::string_view(run->id)) != visited.begin() + static_cast<std::ptrdiff_t>(depth)) return false;
				visited[depth] = run->id;
				if (capability.agent_run_id.empty()) owned = owned || capability.owned_agent_run_ids.contains(run->id);
				else if (depth != 0 && run->id == capability.agent_run_id) owned = true;
				if (run->parent_run_id.empty()) return owned;
				run = FindRun(app, run->parent_run_id);
				if (!run) return false;
			}
			return false; // The existing maximum delegation depth cannot justify a longer chain.
		}

		struct WaitTranscriptView
		{
			const ChatSession* chat = nullptr;
			std::shared_ptr<const std::uint8_t> birth;
		};

		WaitTranscriptView WaitTranscript(const AppState& app, const AgentRun& run, std::string_view root_id,
		    std::string_view host_id)
		{
			if (run.root_chat_id != root_id || app.chats.size() > ManagedSubagentControlService::MaxChatMetadata ||
			    app.agent_runs.size() > ManagedSubagentControlService::MaxLedgerRuns ||
			    std::ranges::count(app.agent_runs, run.id, &AgentRun::id) != 1 ||
			    std::ranges::count(app.agent_runs, run.transcript_chat_id, &AgentRun::transcript_chat_id) != 1) return {};
			const auto live_count = std::ranges::count(app.chats, run.transcript_chat_id, &ChatSession::id);
			if (live_count != 0)
			{
				// Never hide a live replacement behind an older retired birth.
				const auto* chat = live_count == 1 ? ChatDomainService().FindChatById(app, run.transcript_chat_id) : nullptr;
				return {chat, chat ? chat->metadata_incarnation : nullptr};
			}
			const bool terminal = run.status == "completed" || run.status == "failed" ||
			                      run.status == "cancelled" || run.status == "interrupted";
			const auto& ownership = app.hidden_agent_chat_ownership;
			if (!terminal || ownership.data_root != app.data_root ||
			    ownership.chats.size() > ManagedSubagentControlService::MaxChatMetadata ||
			    std::ranges::count(ownership.chats, run.transcript_chat_id, &ChatSession::id) != 1) return {};
			const auto chat = std::ranges::find(ownership.chats, run.transcript_chat_id, &ChatSession::id);
			const auto& proof = chat->managed_terminal_retirement_witness;
			if (!proof || !proof->original_birth || proof->data_root != app.data_root ||
			    proof->run != ManagedTerminalRetirementWitness::RunIdentity::Capture(run) ||
			    chat->agent_run_id != run.id || chat->provider_id != run.provider_id || chat->provider_id != proof->provider_id ||
			    chat->execution_host_id != host_id || chat->execution_host_id != proof->host_id ||
			    chat->workspace_directory != proof->workspace || chat->workspace_worktree_directory != proof->worktree ||
			    chat->native_session_id != proof->native_session_id || chat->created_at != proof->created_at) return {};
			return {&*chat, proof->original_birth};
		}

		bool WaitCurrent(const AppState& app, const UamControlCapability& capability, const UamAgentWait& wait)
		{
			if (!wait.publish_lease->load(std::memory_order_acquire) || app.data_root.native() != wait.data_root.native() ||
			    UamAgentWait::Clock::now() >= wait.publication_deadline || capability.id != wait.capability_id ||
			    capability.directory != wait.directory || capability.chat_id != wait.root_id ||
			    capability.session_chat_id != wait.session_chat_id || capability.provider_id != wait.provider_id ||
			    !capability.mcp_initialized || capability.mcp_connection_id != wait.connection_id ||
			    !HasAuthority(app, capability, time::SystemEpochMillisecondsNow())) return false;
			const ChatSession* root = ChatDomainService().FindChatById(app, wait.root_id);
			const ChatSession* chat = ChatDomainService().FindChatById(app, wait.session_chat_id);
			const AcpSessionState* session = WaitSession(app, capability);
			if (!ControlEnabled(root, capability, true) || !ControlEnabled(chat, capability, false) ||
			    root->imported_read_only || chat->imported_read_only ||
			    !capability.wait_host_at_launch || !execution_hosts::SameConnection(*capability.wait_host_at_launch, wait.host) ||
			    root->workspace_directory != capability.wait_root_workspace_at_launch || root->workspace_worktree_directory != capability.wait_root_worktree_at_launch ||
			    chat->workspace_directory != capability.wait_workspace_at_launch || chat->workspace_worktree_directory != capability.wait_worktree_at_launch ||
			    root->metadata_incarnation != capability.wait_root_incarnation_at_launch || chat->metadata_incarnation != capability.wait_chat_incarnation_at_launch ||
			    root->provider_id != wait.root_provider_id || root->model_id != wait.root_model_id || root->native_session_id != wait.root_native_id ||
			    chat->provider_id != wait.chat_provider_id || chat->model_id != wait.chat_model_id || capability.agent_run_id != wait.origin_agent_run_id ||
			    root->metadata_incarnation != wait.root_incarnation || chat->metadata_incarnation != wait.chat_incarnation ||
			    root->workspace_directory != wait.root_workspace || root->workspace_worktree_directory != wait.root_worktree ||
			    root->execution_host_id != wait.host.id || chat->execution_host_id != wait.host.id ||
			    chat->workspace_directory != wait.workspace || chat->workspace_worktree_directory != wait.worktree ||
			    chat->native_session_id != wait.native_session_id || session != wait.original_session || !session ||
			    session->permission_review_incarnation != wait.launch_incarnation || !session->running || !session->processing ||
			    session->cancel_requested || session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed ||
			    session->session_id != wait.session_id || session->last_process_id != wait.process_id ||
			    session->process_execution_host_id != wait.process_host_id || session->process_execution_host_id != wait.host.id || session->remote_output_delivery_token != wait.remote_delivery_token ||
			    session->codex_turn_id != wait.native_turn_id || session->turn_serial != wait.turn_serial) return false;
			if (!wait.sealed_response.empty() && !std::ranges::any_of(root->uam_control_audit, [&](const auto& record)
			{
				const auto& audit = wait.audit;
				return record.request_id == audit.request_id && record.method == audit.method && record.result == audit.result &&
				    record.reason == audit.reason && record.provider_id == audit.provider_id && record.agent_id == audit.agent_id &&
				    record.run_id == audit.run_id && record.created_at == audit.created_at;
			})) return false;
			const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, wait.host.id);
			if (!host || !execution_hosts::SameConnection(*host, wait.host)) return false;
			const AgentRun* selected = FindRun(app, wait.run_id);
			if (!selected || !IsWaitOwnedDescendant(app, capability, *selected)) return false;
			for (const auto& identity : wait.ancestry)
			{
				const AgentRun* run = FindRun(app, identity.id);
				const auto view = run ? WaitTranscript(app, *run, wait.root_id, identity.host_id) : WaitTranscriptView{};
				const ChatSession* transcript = view.chat;
				if (!run || !transcript || run->root_chat_id != identity.root_id || run->parent_run_id != identity.parent_id ||
				    run->transcript_chat_id != identity.transcript_id || run->provider_id != identity.provider_id ||
				    run->model_id != identity.model_id || run->definition_hash != identity.definition_hash || run->created_at != identity.created_at ||
				    run->agent_id != identity.agent_id || run->execution_capability != identity.execution_capability ||
				    run->resumed_from_run_id != identity.resumed_from_id || run->result_delivery_id != identity.result_delivery_id ||
				    run->effective_workspace_access != identity.workspace_access || run->definition_snapshot != identity.definition_snapshot ||
				    view.birth != identity.transcript_incarnation || transcript->agent_run_id != identity.id ||
				    transcript->execution_host_id != identity.host_id || transcript->provider_id != identity.provider_id) return false;
			}
			return true;
		}

		void RetireAgentWait(AppState& app, UamControlCapability& capability)
		{
			if (!capability.pending_agent_wait) return;
			const auto wait = std::move(capability.pending_agent_wait);
			wait->publish_lease->store(false, std::memory_order_release);
			wait->phase = UamAgentWait::Phase::Retired;
			// Admission globally bounds live plus retired waits; never drop an in-flight future.
			app.uam_wait_retirements.push_back(wait);
		}

		bool RetireCapabilityDirectory(AppState& app, UamControlCapability& capability)
		{
			RetireAgentWait(app, capability);
			const bool writing = std::ranges::any_of(app.uam_wait_retirements, [&](const auto& wait)
			{ return wait->directory == capability.directory && wait->response_future.valid(); });
			if (!writing) (void)paths::RemoveTreeWithoutFollowingLinksNoThrow(capability.directory);
			return !writing;
		}

		void HarvestWaitRetirements(AppState& app, bool shutdown = false)
		{
			for (auto it = app.uam_wait_retirements.begin(); it != app.uam_wait_retirements.end();)
			{
				const auto& wait = *it;
				if (wait->response_future.valid())
				{
					if (!shutdown && wait->response_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
					try { wait->response_result = wait->response_future.get(); wait->response_observed = true; }
					catch (...) { wait->response_result.outcome_unknown = true; wait->response_result.write.error = "Retired control response publisher failed."; }
				}
				const bool capability_exists = std::ranges::any_of(app.uam_control_capabilities, [&](const auto& capability)
				{ return capability.id == wait->capability_id && capability.directory == wait->directory; });
				if (!capability_exists) (void)paths::RemoveTreeWithoutFollowingLinksNoThrow(wait->directory);
				// A revoked still-existing capability may have a raced original reply, never a successor reply.
				else (void)paths::RemoveFileNoThrow(wait->directory / "responses" / (wait->request_id + ".json"));
				it = app.uam_wait_retirements.erase(it);
			}
		}

		ToolResult AgentWait(AppState& app, UamControlCapability& capability, const std::string& request_id,
		    const nlohmann::json& arguments)
		{
			if (capability.wait_cancellation_overflow || capability.cancelled_wait_ids.contains(request_id))
				return {.error = "This agent wait was cancelled or its connection requires renewal."};
			if (capability.pending_agent_wait) return {.error = "One agent wait is already pending for this capability."};
			const auto active_count = std::ranges::count_if(app.uam_control_capabilities, [](const auto& candidate) { return bool(candidate.pending_agent_wait); });
			if (active_count + app.uam_wait_retirements.size() >= kMaxActiveWaits)
				return {.error = "The bounded control wait queue is full. Inspect the run or wait again later."};
			const std::string run_id = arguments.at("runId");
			const AgentRun* run = FindRun(app, run_id);
			if (!run || !IsWaitOwnedDescendant(app, capability, *run)) return {.error = "Agent run is outside this capability's descendant scope."};
			const AcpSessionState* session = WaitSession(app, capability);
			const ChatSession* chat = ChatDomainService().FindChatById(app, capability.session_chat_id);
			const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
			const ExecutionHost* host = chat ? execution_hosts::Find(app.settings.execution_hosts, chat->execution_host_id) : nullptr;
			if (!session || !chat || !root || !host || !session->running || !session->processing || session->turn_serial <= 0 ||
			    session->last_process_id.empty() || !capability.mcp_initialized || capability.mcp_connection_id.empty())
				return {.error = "Agent wait requires a current owned active structured turn and MCP connection."};
			const auto wait = std::make_shared<UamAgentWait>();
			wait->data_root = app.data_root; wait->directory = capability.directory; wait->capability_id = capability.id;
			wait->request_id = request_id; wait->run_id = run_id; wait->root_id = root->id; wait->session_chat_id = chat->id;
			wait->provider_id = capability.provider_id; wait->connection_id = capability.mcp_connection_id;
			wait->root_incarnation = root->metadata_incarnation; wait->chat_incarnation = chat->metadata_incarnation;
			wait->original_session = session; wait->launch_incarnation = session->permission_review_incarnation;
			wait->session_id = session->session_id; wait->native_session_id = chat->native_session_id;
			wait->process_id = session->last_process_id; wait->process_host_id = session->process_execution_host_id;
			wait->remote_delivery_token = session->remote_output_delivery_token; wait->native_turn_id = session->codex_turn_id;
			wait->turn_serial = session->turn_serial; wait->host = *host;
			wait->workspace = chat->workspace_directory; wait->worktree = chat->workspace_worktree_directory;
			wait->root_workspace = root->workspace_directory; wait->root_worktree = root->workspace_worktree_directory;
			wait->root_provider_id = root->provider_id; wait->root_model_id = root->model_id; wait->root_native_id = root->native_session_id;
			wait->chat_provider_id = chat->provider_id; wait->chat_model_id = chat->model_id; wait->origin_agent_run_id = capability.agent_run_id;
			const auto now = UamAgentWait::Clock::now();
			wait->semantic_deadline = now + kAgentWaitSlice;
			// Manager-local absolute cap. Proxy's own earlier absolute timeout must revoke this read.
			wait->publication_deadline = now + kAgentWaitPublicationBudget;
			for (const AgentRun* ancestor = run; ancestor; ancestor = ancestor->parent_run_id.empty() ? nullptr : FindRun(app, ancestor->parent_run_id))
			{
				if (wait->ancestry.size() == 3 || std::ranges::any_of(wait->ancestry, [&](const auto& value) { return value.id == ancestor->id; }))
					return {.error = "Agent ancestry is invalid or outside its bounded depth."};
				if (ancestor->root_chat_id != root->id || (!ancestor->parent_run_id.empty() && !FindRun(app, ancestor->parent_run_id)))
					return {.error = "The owned descendant ancestry is incomplete or foreign."};
				const auto view = WaitTranscript(app, *ancestor, root->id, host->id);
				const ChatSession* transcript = view.chat;
				if (!transcript || transcript->execution_host_id != host->id) return {.error = "The owned child transcript or execution host is unavailable."};
				wait->ancestry.push_back({ancestor->id, ancestor->root_chat_id, ancestor->parent_run_id, ancestor->transcript_chat_id,
				    ancestor->provider_id, ancestor->model_id, ancestor->definition_hash, ancestor->created_at,
				    ancestor->agent_id, ancestor->execution_capability, ancestor->resumed_from_run_id, ancestor->result_delivery_id,
				    ancestor->effective_workspace_access, ancestor->definition_snapshot,
				    view.birth, transcript->execution_host_id});
			}
			if (!WaitCurrent(app, capability, *wait)) return {.error = "The original turn or descendant scope is no longer current."};
			capability.pending_agent_wait = wait;
			return {.ok = true, .deferred = true};
		}

		void ProcessAgentWaits(AppState& app, const std::function<bool(io::AtomicWriteStage)>& response_write_hook)
		{
			HarvestWaitRetirements(app);
			if (std::ranges::any_of(app.uam_control_capabilities, [](const auto& capability)
			    { return capability.pending_agent_wait && capability.pending_agent_wait->phase == UamAgentWait::Phase::AuditPending; }))
				(void)FlushPendingChatSaves(app, false);
			std::size_t actions = 0;
			bool response_busy = std::ranges::any_of(app.uam_wait_retirements, [](const auto& wait) { return wait->response_future.valid(); });
			for (const auto& capability : app.uam_control_capabilities)
				response_busy = response_busy || (capability.pending_agent_wait && capability.pending_agent_wait->response_future.valid());
			for (auto& capability : app.uam_control_capabilities)
			{
				const auto wait = capability.pending_agent_wait;
				if (!wait) continue;
				if (!WaitCurrent(app, capability, *wait)) { RetireAgentWait(app, capability); continue; }
				if (actions == kMaxWaitActionsPerTick) continue;
				if (wait->phase == UamAgentWait::Phase::Waiting)
				{
					const AgentRun* run = FindRun(app, wait->run_id);
					const bool terminal = run->status == "completed" || run->status == "failed" || run->status == "cancelled" || run->status == "interrupted";
					if (!terminal && run->status != "running" && run->status != "queued") { RetireAgentWait(app, capability); continue; }
					if (!terminal && UamAgentWait::Clock::now() < wait->semantic_deadline) continue;
					// Same scoped serializer as immediate inspection; payload is never recomputed after sealing.
					ToolResult observation{.ok = true, .result = AgentStatusPayload(*run)}; // WaitCurrent already proved the complete bounded scope.
					if (!observation.ok || !WaitCurrent(app, capability, *wait)) { RetireAgentWait(app, capability); continue; }
					observation.result["timedOut"] = !terminal;
					wait->sealed_response = nlohmann::json{{"ok", true}, {"result", std::move(observation.result)}}.dump();
					if (wait->sealed_response.size() > kMaxResponseBytes) { RetireAgentWait(app, capability); continue; }
					wait->audit = {AuditText(wait->request_id), "agent_wait", "ok", terminal ? "Read one completed owned descendant." : "Owned descendant is still running; explicit re-wait is permitted.",
					    AuditText(capability.provider_id), AuditText(capability.agent_id), AuditText(capability.agent_run_id), time::TimestampNow()};
					ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
					root->uam_control_audit.push_back(wait->audit);
					if (root->uam_control_audit.size() > kMaxAuditRecords) root->uam_control_audit.erase(root->uam_control_audit.begin());
					wait->phase = UamAgentWait::Phase::AuditPending;
					++actions;
				}
				if (wait->phase == UamAgentWait::Phase::AuditPending)
				{
					if (!wait->audit_ticket && UamAgentWait::Clock::now() >= wait->retry_at)
					{
						if (actions >= kMaxWaitActionsPerTick) continue;
						++actions;
						const std::weak_ptr<UamAgentWait> weak = wait;
						const ChatSession* root = ChatDomainService().FindChatById(app, capability.chat_id);
						wait->audit_ticket = ScheduleControlAuditSave(app, *root, wait->audit, [&app, weak]()
						{
							const auto owner = weak.lock();
							if (!owner) return false;
							const UamControlCapability* current = FindCapability(app, owner->capability_id);
							return current && current->pending_agent_wait == owner && WaitCurrent(app, *current, *owner);
						});
						wait->retry_at = UamAgentWait::Clock::now() + std::chrono::seconds(1);
					}
					const auto receipt = PollControlAuditSave(wait->audit_ticket);
					if (!receipt.finished) continue;
					if (actions >= kMaxWaitActionsPerTick) continue;
					++actions;
					wait->audit_result = receipt;
					if (!receipt.published) { wait->audit_ticket.reset(); continue; }
					if (!receipt.eligible || !WaitCurrent(app, capability, *wait)) { RetireAgentWait(app, capability); continue; }
					wait->phase = UamAgentWait::Phase::ResponsePending;
					wait->retry_at = {};
				}
				if (wait->phase != UamAgentWait::Phase::ResponsePending) continue;
				if (wait->response_future.valid())
				{
					if (wait->response_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
					try { wait->response_result = wait->response_future.get(); wait->response_observed = true; }
					catch (...) { wait->response_result.outcome_unknown = true; wait->response_result.write.error = "Control response publisher failed."; }
					response_busy = false;
					++actions;
					if (!WaitCurrent(app, capability, *wait)) { RetireAgentWait(app, capability); continue; }
					if (wait->response_result.write.success || wait->response_result.write.primary_committed ||
					    wait->response_result.rename_crossed || wait->response_result.outcome_unknown)
					{
						// POSIX may leave both legacy flags false after rename but before directory sync.
						const bool published = wait->response_result.write.success;
						wait->phase = published ? UamAgentWait::Phase::Published : UamAgentWait::Phase::Retired;
						wait->publish_lease->store(false, std::memory_order_release);
						capability.pending_agent_wait.reset();
						if (!published) app.uam_wait_retirements.push_back(wait); // Preserve irreversible/uncertain facts through owned retirement.
						continue;
					}
					wait->retry_at = UamAgentWait::Clock::now() + std::chrono::seconds(1);
				}
				if (response_busy || actions >= kMaxWaitActionsPerTick || UamAgentWait::Clock::now() < wait->retry_at) continue;
				if (!wait->audit_result.published || !wait->audit_result.eligible || !WaitCurrent(app, capability, *wait))
				{ RetireAgentWait(app, capability); continue; }
				const auto path = wait->directory / "responses" / (wait->request_id + ".json");
				const auto lease = wait->publish_lease;
				const auto deadline = wait->publication_deadline;
				std::packaged_task<UamWaitResponsePublication()> publisher([path, lease, deadline, bytes = wait->sealed_response, response_write_hook]()
				{
					UamWaitResponsePublication publication;
					if (!lease->load(std::memory_order_acquire) || UamAgentWait::Clock::now() >= deadline ||
					    paths::IsLinkOrReparsePointNoThrow(path) || paths::IsLinkOrReparsePointNoThrow(path.parent_path())) return publication;
					try
					{
						publication.write = io::AtomicWriteFileDetailed(path, bytes, [&](io::AtomicWriteStage stage)
						{
							if (stage == io::AtomicWriteStage::AfterReplaceBeforeDirectorySync) publication.rename_crossed = true;
							return lease->load(std::memory_order_acquire) && UamAgentWait::Clock::now() < deadline &&
							    (!response_write_hook || response_write_hook(stage));
						});
					}
					catch (...)
					{
						publication.outcome_unknown = true; // Never replay a possibly installed response after an exception.
					}
					return publication;
				});
				wait->response_future = publisher.get_future();
				std::thread(std::move(publisher)).detach();
				response_busy = true;
				++actions;
			}
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
			if (method != "agent_delegate" && ReadOnlyAgent(app, *capability) && (IsMutation(method) || method == "computer_use_request"))
				return reject("This UAM agent has read-only access. Mutation tools are unavailable.");
			if (!capability->seen_request_ids.insert(request_id).second)
				return reject("Control request replay was rejected.");
			if (capability->seen_request_ids.size() > kMaxSeenRequests)
				capability->seen_request_ids.erase(capability->seen_request_ids.begin());



			if (method == "_mcp_cancel_request")
			{
				if (capability->mcp_initialized && capability->mcp_connection_id == arguments.at("connectionId").get<std::string>())
				{
					const std::string cancelled_id = arguments.at("requestId");
					if (capability->pending_agent_wait && capability->pending_agent_wait->request_id == cancelled_id)
						RetireAgentWait(app, *capability);
					if (!capability->cancelled_wait_ids.contains(cancelled_id) &&
					    capability->cancelled_wait_ids.size() == kMaxCancelledWaitsPerConnection)
					{
						capability->wait_cancellation_overflow = true; // No eviction can revive a cancelled ID.
						return reject("The bounded wait cancellation receipt queue is full; renew the connection.");
					}
					capability->cancelled_wait_ids.insert(cancelled_id);
				}
				return {{"ok", true}, {"result", nlohmann::json::object()}};
			}
			// Transport receipts are session-bound observations, not tools or durable chat mutations.
			if (method == "_mcp_initialized" || method == "_mcp_disconnected" || method == "_mcp_heartbeat")
			{
				const std::string connection = arguments["connectionId"].get<std::string>();
				if (method == "_mcp_heartbeat")
				{
					if (!capability->mcp_initialized || capability->mcp_connection_id != connection) return reject("MCP connection is no longer current.");
					capability->mcp_heartbeat_at_epoch_ms = now_epoch_ms;
					return {{"ok", true}, {"result", nlohmann::json::object()}};
				}
				if (method == "_mcp_disconnected" && capability->mcp_connection_id != connection)
					return {{"ok", true}, {"result", nlohmann::json::object()}};
				if (method == "_mcp_disconnected" || capability->mcp_connection_id != connection)
				{
					RetireAgentWait(app, *capability);
					capability->cancelled_wait_ids.clear();
					capability->wait_cancellation_overflow = false;
				}
				capability->mcp_heartbeat_at_epoch_ms = 0;
				capability->mcp_connection_id = connection;
				capability->mcp_initialized = method == "_mcp_initialized";
				capability->mcp_initialized_at_epoch_ms = capability->mcp_initialized ? now_epoch_ms : 0;
				return {{"ok", true}, {"result", nlohmann::json::object()}};
			}

			while (!capability->request_times_epoch_ms.empty() &&
			       capability->request_times_epoch_ms.front() <= now_epoch_ms - 60000)
				capability->request_times_epoch_ms.pop_front();
			if (capability->request_times_epoch_ms.size() >= kMaxRequestsPerMinute)
				return reject("Control capability exceeded the bounded request rate.");
			capability->request_times_epoch_ms.push_back(now_epoch_ms);

			const ChatSession* before_chat = ChatDomainService().FindChatById(app, capability->chat_id);
			const std::optional<ChatSession> rollback = before_chat == nullptr || method == "agent_wait" ? std::nullopt : std::optional<ChatSession>{*before_chat};
			const bool goal_needs_approval = method == "goal_create" && before_chat != nullptr &&
			    !MutationsAllowed(*before_chat);
			ToolResult result = method == "agent_wait"
			                        ? AgentWait(app, *capability, request_id, arguments)
			                        : method == "computer_use_request"
			                        ? ComputerUseRequest(app, *capability, request_id, arguments, now_epoch_ms)
			                        : method == "user_question"
			                            ? UserQuestionRequest(app, *capability, request_id, arguments, now_epoch_ms)
			                        : goal_needs_approval
			                            ? GoalCreateRequest(app, *capability, request_id, arguments, now_epoch_ms)
			                            : ExecuteTool(app, *capability, method, arguments);
			if (result.deferred) return method == "agent_wait" ? nlohmann::json{{"pendingAgentWait", true}} : nlohmann::json{{"pendingApproval", true}};
			const UamControlCapability capability_snapshot = *capability;
			if (!AppendAuditAndSave(app, capability_snapshot, request_id, method, result))
			{
				if (result.ok && method == "agent_delegate")
				{
					std::string cancel_error;
					(void)AgentRunScheduler::CancelTree(app, result.result.value("runId", ""), &cancel_error);
					ManagedParentContinuationService::Revoke(app, capability_snapshot.chat_id, "delegation_save_failed");
				}
				if (rollback.has_value())
				{
					if (ChatSession* chat = ChatDomainService().FindChatById(app, capability_snapshot.chat_id); chat != nullptr)
						*chat = *rollback;
				}
				return reject("Control result could not be durably recorded.");
			}
			if (result.ok && method == "agent_delegate" && result.result.value("resultDelivery", "") == "automatic-pending" &&
			    !ManagedParentContinuationService::DelegationCommitConfirmed(app, capability_snapshot.chat_id))
			{
				ManagedParentContinuationService::Revoke(app, capability_snapshot.chat_id, "activation_changed");
				result.result["resultDelivery"] = "held";
				result.result["resultDeliveryReason"] = "activation_changed";
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
			nlohmann::json goal_create = ToolSchema(
			    "goal_create", {{"objective", {{"type", "string"}}},
			                    {"idempotencyKey", {{"type", "string"}}}},
			    {"objective", "idempotencyKey"});
			goal_create["description"] = "Create one bounded UAM goal. In Plan mode this is unavailable; in YOLO mode it runs immediately; otherwise it waits for the user's approval in UAM. Do not retry after denial or timeout.";
			nlohmann::json user_question = ToolSchema(
			    "user_question", { {"question", {{"type", "string"}}},
			                       {"options", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 3}}} },
			    {"question"});
			user_question["description"] = "Ask the user one bounded question in UAM. The request is shown in the chat and waits for an explicit answer; free text is always available. Do not retry after timeout.";
			nlohmann::json agent_list = ToolSchema("agent_list");
			agent_list["description"] = "List permitted agents and providerTargets on this chat's execution host. Only eligible targets have an installed compatible worker CLI; authentication remains unverified. Same-provider defaultModelId inherits the parent; an empty cross-provider default uses that provider's default.";
			nlohmann::json agent_delegate = MutationToolSchema("agent_delegate", {{"agentId", {{"type", "string"}}}, {"task", {{"type", "string"}}},
			    {"providerId", {{"type", "string"}}}, {"modelId", {{"type", "string"}}}}, {"agentId", "task"});
			agent_delegate["description"] = "Start one permitted agent using optional providerId and modelId from agent_list providerTargets. Omitting providerId inherits the parent provider; switching provider without modelId uses the target provider's default. CLI availability does not verify login. resultDelivery reports cooperative-only, automatic-pending or held. An accepted direct-root delegation may deliver bounded child results after a normal turn end on the SAME still-live local Codex session; it never restarts a parent or replays uncertain input.";
			nlohmann::json agent_wait = ToolSchema("agent_wait", {{"runId", {{"type", "string"}}}}, {"runId"});
			agent_wait["description"] = "Wait in this active turn for one owned child for up to 80 seconds. Returns its exact status/result and timedOut. If still running, another explicit agent_wait on the same run is allowed; do not redelegate. This read never cancels the child or starts a new turn.";
			return nlohmann::json::array({
			    std::move(agent_wait),
			    ToolSchema("skill_list"),
			    ToolSchema("skill_read", {{"id", {{"type", "string"}}}, {"path", {{"type", "string"}, {"description", "Relative asset path inside the installed skill; defaults to SKILL.md."}}}}, {"id"}),
			    std::move(agent_list),
			    ToolSchema("agent_status", {{"runId", {{"type", "string"}}}}, {"runId"}),
			    ToolSchema("goal_get"),
			    std::move(agent_delegate),
			    MutationToolSchema("agent_cancel", {{"runId", {{"type", "string"}}}}, {"runId"}),
			    std::move(goal_create),
			    std::move(user_question),
			    { {"name", "computer_use_request"},
			      {"description", "Ask the user in UAM to enable Computer Use for this local chat. The tool waits for an explicit Allow or Deny decision; it never approves itself. Do not retry after denial or timeout."},
			      {"inputSchema", {{"type", "object"}, {"properties", {{"reason", {{"type", "string"}}}}},
			                       {"required", nlohmann::json::array({"reason"})}, {"additionalProperties", false}}} },
			});
		}

		std::optional<nlohmann::json> SubmitToManager(const std::filesystem::path& capability,
		                                              std::string_view method,
		                                              const nlohmann::json& arguments, std::stop_token cancel = {},
		                                              const std::function<WaitAbortReason()>& wait_probe = {},
		                                              std::string* request_id_out = nullptr,
		                                              WaitAbortReason* abort_out = nullptr)
		{
			const bool wait_method = method == "agent_wait";
			const bool cancel_receipt = method == "_mcp_cancel_request";
			const auto deadline = std::chrono::steady_clock::now() +
			    std::chrono::milliseconds(wait_method ? 100000 : cancel_receipt ? 2000 : 0);
			if (abort_out != nullptr) *abort_out = WaitAbortReason::None;
			const auto aborted = [&]()
			{
				WaitAbortReason reason = wait_method && wait_probe ? wait_probe() : WaitAbortReason::None;
				if (reason == WaitAbortReason::None && wait_method && std::chrono::steady_clock::now() >= deadline)
					reason = WaitAbortReason::Deadline;
				if (abort_out != nullptr && reason != WaitAbortReason::None) *abort_out = reason;
				return reason != WaitAbortReason::None;
			};
			if (!uam::paths::IsDirectoryNoThrow(capability) || uam::paths::IsLinkOrReparsePointNoThrow(capability))
				return std::nullopt;
			// Independent stdio clients must finish their file IO before a stopped transfer.
			std::error_code path_error;
			const auto absolute_capability = std::filesystem::absolute(capability, path_error);
			if (path_error || absolute_capability.parent_path().parent_path().filename() != "uam-control")
				return std::nullopt;
			const auto root = absolute_capability.parent_path().parent_path().parent_path();
			std::string ownership_error;
			const auto writer = platform::DataFolderWriterLease::TryAcquire(root, platform::DataFolderWriterLease::Access::Shared, ownership_error);
			if (!writer || !DataFolderTransition::CheckExternalSourceFence(root, ownership_error, true))
				return std::nullopt;
			const std::string id = RandomRequestId();
			if (request_id_out != nullptr) *request_id_out = id;
			if (aborted()) return std::nullopt;
			const nlohmann::json request{{"requestId", id}, {"method", method}, {"arguments", arguments}};
			const std::string request_text = request.dump();
			if (request_text.size() > kMaxRequestBytes ||
			    !uam::io::WriteTextFile(capability / "requests" / (id + ".json"), request_text)) return std::nullopt;
			const std::filesystem::path response_path = capability / "responses" / (id + ".json");
			const int max_attempts = method == "computer_use_request" || method == "goal_create" || method == "user_question"
			    ? 4000 : 400;
			for (int attempt = 0; !cancel.stop_requested(); ++attempt)
			{
				if (aborted()) break;
				if (wait_method || cancel_receipt)
				{
					if (std::chrono::steady_clock::now() >= deadline) break;
				}
				else if (attempt >= max_attempts) break; // All other methods retain existing budgets.
				const auto size = uam::paths::FileSizeNoThrow(response_path);
				if (size.has_value())
				{
					if (*size == 0 || *size > kMaxResponseBytes || uam::paths::IsLinkOrReparsePointNoThrow(response_path)) return std::nullopt;
					std::string text;
					if (!uam::io::TryReadTextFile(response_path, text) || text.size() != *size) return std::nullopt;
					uam::paths::RemoveFileNoThrow(response_path);
					const nlohmann::json response = nlohmann::json::parse(text, nullptr, false);
					if (!response.is_object() || !response.contains("ok") || !response["ok"].is_boolean()) return std::nullopt;
					if (response["ok"].get<bool>() ? !response.contains("result") :
					    (response.contains("error") && !response["error"].is_string())) return std::nullopt;
					if (aborted()) return std::nullopt;
					return response;
				}
				if (!uam::paths::IsDirectoryNoThrow(capability)) return std::nullopt;
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				std::this_thread::sleep_for(wait_method || cancel_receipt
				    ? std::max(std::chrono::milliseconds(0), std::min(std::chrono::milliseconds(25), remaining))
				    : std::chrono::milliseconds(25));
			}
			if (method == "_mcp_heartbeat")
			{
				(void)uam::paths::RemoveFileNoThrow(capability / "requests" / (id + ".json"));
				(void)uam::paths::RemoveFileNoThrow(response_path);
			}
			return std::nullopt;
		}

		void CancelWait(const std::filesystem::path& capability, const std::string& connection,
		                const std::string& original_request)
		{
			if (original_request.empty()) return;
			// A timed-out receipt is NOT proof of retirement. Never print/adopt the abandoned payload.
			(void)SubmitToManager(capability, "_mcp_cancel_request",
			    {{"connectionId", connection}, {"requestId", original_request}});
			// The manager retires the response; do not perform unleased cleanup after SubmitToManager.
		}

	} // namespace

	bool UamControlService::IsStdioServerInvocation(const std::vector<std::string>& arguments)
	{
		return std::ranges::find(arguments, kServerFlag) != arguments.end();
	}

	bool UamControlService::EnabledForChat(const AppSettings& settings, const ChatSession& chat)
	{
		return !chat.uam_control_explicitly_disabled && central_configuration::McpEnabled(
		    settings.central_provider_configuration, chat.provider_id, chat.execution_host_id, "uam-mcp", chat.uam_control_enabled);
	}

	bool UamControlService::RequiresSessionStopForEnable(const AcpSessionState* session, bool apply_on_next_launch)
	{
		return !apply_on_next_launch && session != nullptr && session->running && session->uam_control_capability_id.empty();
	}

	bool UamControlService::SupportsStructuredProtocol(std::string_view protocol)
	{
		return protocol == uam::provider_profile_constants::kProtocolCodexAppServer ||
		       protocol == uam::provider_profile_constants::kProtocolGeminiAcp ||
		       protocol == uam::provider_profile_constants::kProtocolOpenCodeAcp ||
		       protocol == uam::provider_profile_constants::kProtocolCopilotAcp ||
		       protocol == uam::provider_profile_constants::kProtocolCopilotSdk ||
		       protocol == uam::provider_profile_constants::kProtocolClaudeCodeStreamJson ||
		       protocol == uam::provider_profile_constants::kProtocolAntigravityStreamJson;
	}

		int UamControlService::RunStdioServerFromEnvironment()
	{
		const auto capability = uam::env::GetTrimmedPath(kCapabilityDirectoryEnvironment);
		if (!capability.has_value()) return 2;
		const std::string connection_id = RandomRequestId();
		bool initialization_replied = false;
		bool connection_recorded = false;
		std::jthread heartbeat;
		StdioInputPump input(PlatformServicesFactory::Instance().process_service);
		int exit_code = 0;
		std::string line;
		while (input.Next(line))
		{
			const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
			if (!request.is_object()) continue;
			if (!request.contains("id"))
			{
				if (initialization_replied && request.contains("method") && request["method"].is_string() && request["method"] == "notifications/initialized")
				{
					const std::optional<nlohmann::json> receipt = SubmitToManager(*capability, "_mcp_initialized", {{"connectionId", connection_id}});
					connection_recorded = receipt.has_value() && receipt->value("ok", false);
					if (connection_recorded && !heartbeat.joinable())
					{
						heartbeat = std::jthread([directory = *capability, connection_id](std::stop_token cancel)
						{
							std::condition_variable_any wake;
							std::mutex mutex;
							std::unique_lock<std::mutex> lock(mutex);
							while (!cancel.stop_requested())
							{
								try
								{
									const std::optional<nlohmann::json> result = SubmitToManager(directory, "_mcp_heartbeat", {{"connectionId", connection_id}}, cancel);
									if (cancel.stop_requested() || !uam::paths::IsDirectoryNoThrow(directory) ||
									    (result.has_value() && result->value("error", "") == "MCP connection is no longer current.")) break;
								}
								catch (const std::exception&) { break; }
								wake.wait_for(lock, cancel, std::chrono::seconds(5), [] { return false; });
							}
						});
					}

				}
				continue;
			}
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
					const std::string name = request["params"]["name"].get<std::string>();
					const bool wait_call = name == "agent_wait";
					if (wait_call) input.BeginWait(id);
					std::string wait_request_id;
					WaitAbortReason abort = WaitAbortReason::None;
					const auto manager_response = SubmitToManager(*capability, name, request["params"]["arguments"], {},
					    wait_call ? std::function<WaitAbortReason()>([&] { return input.ProbeWait(); }) : std::function<WaitAbortReason()>{},
					    wait_call ? &wait_request_id : nullptr, wait_call ? &abort : nullptr);
					if (wait_call)
					{
						const WaitAbortReason latest = input.ProbeWait();
						if (latest != WaitAbortReason::None) abort = latest;
						if (!manager_response || abort != WaitAbortReason::None)
						{
							CancelWait(*capability, connection_id, wait_request_id);
							const WaitAbortReason after_cancel = input.ProbeWait();
							if (after_cancel != WaitAbortReason::None) abort = after_cancel;
						}
						input.EndWait();
						if (abort == WaitAbortReason::InputClosed || abort == WaitAbortReason::InputError) break;
						if (abort == WaitAbortReason::Cancelled) continue; // MCP cancellation has no reply.
					}
					const bool ok = abort == WaitAbortReason::None && manager_response.has_value() && manager_response->value("ok", false);
					const nlohmann::json payload = ok ? (*manager_response)["result"] : nlohmann::json{{"error", abort == WaitAbortReason::Deadline ? "Agent wait transport timed out; the wait was not retried." : manager_response.has_value() ? manager_response->value("error", "Control request failed.") : "Control manager is unavailable."}};
					response["result"] = {{"content", nlohmann::json::array({{{"type", "text"}, {"text", payload.dump()}}})}, {"isError", !ok}};
				}
			}
			else
			{
				response["error"] = {{"code", -32601}, {"message", "Method not found."}};
			}
			const std::string output = response.dump();
			if (output.size() > kMaxResponseBytes) { exit_code = 2; break; }
			std::cout << output << '\n' << std::flush;
			if (method == "initialize" && std::cout.good()) initialization_replied = true;
		}
		heartbeat.request_stop();
		if (heartbeat.joinable()) heartbeat.join();
		if (connection_recorded && uam::paths::IsDirectoryNoThrow(*capability))
		{
			// Do not delay provider shutdown waiting for the GUI to process the receipt.
			const std::string id = RandomRequestId();
			(void)uam::io::WriteTextFile(*capability / "requests" / (id + ".json"),
			    nlohmann::json{{"requestId", id}, {"method", "_mcp_disconnected"}, {"arguments", {{"connectionId", connection_id}}}}.dump());
		}
		return input.Failed() ? 2 : exit_code;
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
		for (const auto& session : app.acp_sessions)
			if (session) ManagedParentContinuationService::Revoke(app, session->chat_id, "control_shutdown");
		for (auto& capability : app.uam_control_capabilities) RetireAgentWait(app, capability);
		for (auto& session : app.acp_sessions)
			if (session != nullptr) session->uam_control_capability_id.clear();
		app.uam_control_capabilities.clear();
		// Explicit shutdown barrier only; normal owner polling never waits for IO.
		HarvestWaitRetirements(app, true);
		if (!app.data_root.empty() && !app.uam_control_manager_id.empty())
			(void)uam::paths::RemoveTreeWithoutFollowingLinksNoThrow(ControlRoot(app));
		app.uam_control_manager_id.clear();
	}

	bool UamControlService::AppendSessionMcpServer(AppState& app, AcpSessionState& session,
	                                               const ChatSession& chat, std::string_view setup_method,
	                                               nlohmann::json& request, std::string* error_out)
	{
		if (!EnabledForChat(app.settings, chat) || session.model_discovery_only) return true;
		if (setup_method != uam::acp_methods::kSessionNew && setup_method != uam::acp_methods::kSessionLoad && setup_method != uam::acp_methods::kSessionResume && setup_method != "session.create" && setup_method != "session.resume")
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
		if (root_chat == nullptr || !EnabledForChat(app.settings, *root_chat))
		{
			if (error_out != nullptr) *error_out = "The owning root chat no longer permits UAM control.";
			return false;
		}
		UamControlCapability capability;
		capability.id = NewUuid();
		capability.directory = ControlRoot(app) / app.uam_control_manager_id / capability.id;
		capability.chat_id = root_chat_id;
		capability.session_control_override_at_launch = !chat.uam_control_enabled;
		capability.root_control_override_at_launch = !root_chat->uam_control_enabled;
		capability.session_chat_id = chat.id;
		if (const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id))
			capability.wait_host_at_launch = *host;
		capability.wait_root_incarnation_at_launch = root_chat->metadata_incarnation;
		capability.wait_chat_incarnation_at_launch = chat.metadata_incarnation;
		capability.wait_workspace_at_launch = chat.workspace_directory;
		capability.wait_worktree_at_launch = chat.workspace_worktree_directory;
		capability.wait_root_workspace_at_launch = root_chat->workspace_directory;
		capability.wait_root_worktree_at_launch = root_chat->workspace_worktree_directory;
		capability.provider_id = session.provider_id;
		capability.agent_id = uam::strings::NonEmptyOrFallback(session.active_uam_agent_id, "build");
		capability.agent_run_id = run == nullptr ? std::string{} : run->id;
		capability.agent_skills = session.active_uam_agent_skills;
		capability.provider_configuration_directory = session.provider_configuration_directory;
		capability.central_skills_enabled = central_configuration::McpEnabled(app.settings.central_provider_configuration, chat.provider_id, chat.execution_host_id, "shared-setup", app.settings.central_provider_configuration.enabled) && (capability.agent_id == "build" || capability.agent_id == "plan");
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

	bool UamControlService::PrepareTerminalMcpServer(AppState& app, CliTerminalState& terminal, const ChatSession& chat, nlohmann::json& server, std::string& error)
	{
		if (terminal.uam_control_session != nullptr) RevokeForSession(app, *terminal.uam_control_session);
		terminal.uam_control_session.reset();
		terminal.uam_control_relay.reset();
		if (!EnabledForChat(app.settings, chat)) return true;
		const AgentDefinitionCatalog catalog = AgentCatalog(app, chat);
		const auto selected = std::ranges::find_if(catalog.definitions, [&](const AgentDefinition& agent) { return agent.id == chat.uam_agent_id; });
		if (selected == catalog.definitions.end()) { error = "The selected UAM agent is unavailable."; return false; }
		terminal.uam_control_session = std::make_shared<AcpSessionState>();
		AcpSessionState& session = *terminal.uam_control_session;
		session.chat_id = chat.id;
		session.provider_id = chat.provider_id;
		session.active_uam_agent_id = selected->id;
		session.active_uam_agent_workspace_access = selected->workspace_access;
		session.active_uam_agent_skills = selected->skills;
		session.active_uam_agent_delegates = selected->delegates;
		nlohmann::json request = {{"params", {{"mcpServers", nlohmann::json::array()}}}};
		if (!AppendSessionMcpServer(app, session, chat, uam::acp_methods::kSessionNew, request, &error)) return false;
		server = request["params"]["mcpServers"].back();
		return true;
	}

	void UamControlService::RevokeForChat(AppState& app, std::string_view chat_id)
	{
		std::set<std::string> ids;
		for (const UamControlCapability& capability : app.uam_control_capabilities)
			if (capability.chat_id == chat_id || capability.session_chat_id == chat_id) ids.insert(capability.id);
		for (const std::unique_ptr<AcpSessionState>& session : app.acp_sessions)
			if (session != nullptr && ids.contains(session->uam_control_capability_id)) RevokeForSession(app, *session);
		for (const std::unique_ptr<CliTerminalState>& terminal : app.cli_terminals)
			if (terminal != nullptr && terminal->uam_control_session != nullptr && ids.contains(terminal->uam_control_session->uam_control_capability_id))
			{
				RevokeForSession(app, *terminal->uam_control_session);
				terminal->uam_control_relay.reset();
			}
		for (auto& capability : app.uam_control_capabilities)
			if (ids.contains(capability.id)) (void)RetireCapabilityDirectory(app, capability);
		std::erase_if(app.uam_control_capabilities, [&](const UamControlCapability& capability)
		{ return ids.contains(capability.id); });
	}

	void UamControlService::RevokeForSession(AppState& app, AcpSessionState& session)
	{
		ManagedParentContinuationService::Revoke(app, session.chat_id, "control_revoked");
		const std::string id = session.uam_control_capability_id;
		for (auto& capability : app.uam_control_capabilities)
			if (capability.id == id) (void)RetireCapabilityDirectory(app, capability);
		session.uam_control_relay.reset();
		if (id.empty()) return;
		session.uam_control_capability_id.clear();
		std::erase_if(app.uam_control_capabilities, [&](const UamControlCapability& capability)
		{ return capability.id == id; });
	}

	std::optional<AcpPendingUserInputState> UamControlService::PendingApprovalForChat(
	    const AppState& app, std::string_view chat_id)
	{
		const int64_t now = uam::time::SystemEpochMillisecondsNow();
		for (const UamControlCapability& capability : app.uam_control_capabilities)
		{
			if (!capability.pending_approval.has_value() || ApprovalChatId(capability) != chat_id ||
			    capability.pending_approval->expires_at_epoch_ms <= now ||
			    !HasAuthority(app, capability, now) || !ComputerUseApprovalCurrent(app, capability)) continue;
			const ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id));
			if (chat == nullptr) continue;
			AcpPendingUserInputState input;
			input.request_id_json = ApprovalRequestId(capability);
			input.status = "pending";
			input.attention_kind = "question";
			AcpUserInputQuestionState question;
			const bool computer_use = capability.pending_approval->method == "computer_use_request";
			const bool user_question = capability.pending_approval->method == "user_question";
			question.id = computer_use ? "computerUse" : user_question ? "userQuestion" : "goalCreate";
			question.header = computer_use ? "Computer Use" : user_question ? "Question" : "Goal";
			if (computer_use)
				question.question = "Allow " + uam::strings::SafeLine(capability.provider_id, 64, true) +
				                   " to enable Computer Use in " + uam::strings::SafeLine(chat->title, 120, true) +
				                   "? " + capability.pending_approval->reason;
			else if (user_question)
				question.question = uam::strings::SafeLine(capability.pending_approval->question, 2048, true);
			else
				question.question = "Allow " + uam::strings::SafeLine(capability.provider_id, 64, true) +
				                   " to create this goal in " + uam::strings::SafeLine(chat->title, 120, true) +
				                   "? " + uam::strings::SafeLine(capability.pending_approval->objective, 2048, true);
			if (user_question)
			{
				for (const std::string& option : capability.pending_approval->question_options)
					question.options.push_back({option, {}});
				question.is_other = true;
			}
			else question.options = computer_use
			    ? std::vector<AcpUserInputOptionState>{{"Allow", "Enable Computer Use for this chat."},
			                                          {"Deny", "Keep Computer Use disabled."}}
			    : std::vector<AcpUserInputOptionState>{{"Allow", "Create this goal."},
			                                          {"Deny", "Do not create this goal."}};
			input.questions.push_back(std::move(question));
			return input;
		}
		return std::nullopt;
	}

	bool UamControlService::ResolveApproval(
	    AppState& app, std::string_view chat_id, std::string_view request_id,
	    const std::map<std::string, std::vector<std::string>>& answers,
	    std::string* error_out)
	{
		auto fail = [&](std::string message) {
			if (error_out != nullptr) *error_out = std::move(message);
			return false;
		};
		const int64_t now = uam::time::SystemEpochMillisecondsNow();
		for (UamControlCapability& capability : app.uam_control_capabilities)
		{
			if (!capability.pending_approval.has_value() || ApprovalRequestId(capability) != request_id)
				continue;
			if (ApprovalChatId(capability) != chat_id)
				return fail("This UAM request is no longer active in this chat.");
			const bool authority_current = HasAuthority(app, capability, now) && capability.pending_approval->expires_at_epoch_ms > now;
			if ((!authority_current && capability.pending_approval->method == "computer_use_request") ||
			    !ComputerUseApprovalCurrent(app, capability))
			{
				(void)WriteResponse(capability.directory, capability.pending_approval->request_id,
				    {{"ok", false}, {"error", "The original computer-use turn or launch authority changed. Request approval again."}});
				capability.pending_approval.reset();
				return fail("This computer-use approval no longer belongs to the active user turn.");
			}
			if (!authority_current) return fail("This UAM request is no longer active in this chat.");
			const bool computer_use = capability.pending_approval->method == "computer_use_request";
			const bool user_question = capability.pending_approval->method == "user_question";
			const std::string question_id = computer_use ? "computerUse" : user_question ? "userQuestion" : "goalCreate";
			if (answers.size() != 1 || !answers.contains(question_id) ||
			    answers.at(question_id).size() != 1)
				return fail(user_question ? "Provide one answer to this UAM question." : "Choose Allow or Deny for this UAM request.");
			const std::string& answer = answers.at(question_id).front();
			if (user_question)
			{
				if (!SafeText(answer, 2048) || uam::strings::Trim(answer).empty())
					return fail("Provide one non-empty answer to this UAM question.");
			}
			else if (answer != "Allow" && answer != "Deny")
				return fail("Choose Allow or Deny for this UAM request.");
			if (computer_use && !app.settings.computer_use_globally_enabled)
				return fail("Computer Use is disabled globally.");
			ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id));
			ChatSession* root_chat = ChatDomainService().FindChatById(app, capability.chat_id);
			if (chat == nullptr || !ControlEnabled(root_chat, capability, true) ||
			    ((user_question || computer_use) && !ControlEnabled(chat, capability, false)) ||
			    (computer_use && (!uam::computer_use::AvailableForChat(*chat) ||
			                      !uam::computer_use::UsesUamBackend(*chat))))
				return fail("This UAM request is no longer available for this chat.");
			if (!user_question && answer == "Allow" &&
			    uam::approval_modes::AppApprovalModeOrEmpty(chat->approval_mode) ==
			        uam::approval_modes::kPlanApprovalMode)
				return fail("This request cannot be approved in Plan mode.");
			const ChatSession before = *chat;
			const ChatSession root_before = *root_chat;
			chat->interaction_at = uam::time::InteractionTimestampNow();
			ToolResult result;
			if (user_question)
			{
				result.ok = true;
				result.result = {{"answer", uam::strings::Trim(answer)}};
				result.reason = "The user answered the UAM question.";
			}
			else if (answer == "Allow")
			{
				if (computer_use)
				{
					chat->computer_use_enabled = true;
					std::string error;
					if (!ComputerUseService::PersistTrustedTask(app, chat->id, capability.pending_approval->computer_use_task, &error) ||
					    !ComputerUseService::SetControlState(app, chat->id, "armed", &error))
					{
						(void)ComputerUseService::SetControlState(app, chat->id, "stopped");
						*chat = before;
						result.error = error.empty() ? "Computer Use could not be enabled." : error;
					}
					else
					{
						result.ok = true;
						result.result = {{"enabled", true}, {"chatId", chat->id}};
						result.reason = "The user approved Computer Use for this chat.";
					}
				}
				else
				{
					result = GoalCreate(app, capability,
					                    {{"objective", capability.pending_approval->objective},
					                     {"idempotencyKey", capability.pending_approval->idempotency_key}});
				}
			}
			else result.error = computer_use
			    ? "The user denied Computer Use for this chat. Do not retry without a new user request."
			    : "The user denied goal creation for this chat. Do not retry without a new user request.";
			const nlohmann::json response = result.ok
			    ? nlohmann::json{{"ok", true}, {"result", result.result}}
			    : nlohmann::json{{"ok", false}, {"error", result.error}};
			if (!AppendAuditAndSave(app, capability, capability.pending_approval->request_id,
			                        capability.pending_approval->method, result) ||
			    !WriteResponse(capability.directory, capability.pending_approval->request_id, response))
			{
				if (result.ok && computer_use)
					(void)ComputerUseService::SetControlState(app, chat->id, "stopped");
				*chat = before;
				if (chat != root_chat) *root_chat = root_before;
				(void)ChatRepository::SaveChat(app.data_root, *chat);
				if (chat != root_chat) (void)ChatRepository::SaveChat(app.data_root, *root_chat);
				return fail("UAM decision could not be delivered. Try again.");
			}
			capability.pending_approval.reset();
			return true;
		}
		return fail("This UAM request is no longer pending.");
	}

	bool UamControlService::ProcessPendingRequests(AppState& app)
	{
		return ProcessPendingRequests(app, {});
	}

	bool UamControlService::ProcessPendingRequests(AppState& app,
	    const std::function<bool(io::AtomicWriteStage)>& response_write_hook)
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
				// Keep only the actual owned launch reservation. It cannot process requests yet.
				if (acp_detail::ShouldRetainPendingLocalAcpControlCapability(app, capability, now, GetAppTimeSeconds()))
				{
					++capability_index;
					continue;
				}
				(void)RetireCapabilityDirectory(app, capability);
				for (auto& session : app.acp_sessions)
					if (session != nullptr && session->uam_control_capability_id == capability.id)
						session->uam_control_capability_id.clear();
				app.uam_control_capabilities.erase(app.uam_control_capabilities.begin() + static_cast<std::ptrdiff_t>(capability_index));
				changed = true;
				continue;
			}
			if (capability.pending_approval.has_value() &&
			    (capability.pending_approval->expires_at_epoch_ms <= now || !ComputerUseApprovalCurrent(app, capability)))
			{
				const ToolResult expired{.error = capability.pending_approval->expires_at_epoch_ms <= now
				    ? "UAM approval timed out." : "The original computer-use turn or launch authority changed. Request approval again."};
				(void)AppendAuditAndSave(app, capability, capability.pending_approval->request_id,
				                         capability.pending_approval->method, expired);
				(void)WriteResponse(capability.directory, capability.pending_approval->request_id,
				                    {{"ok", false}, {"error", expired.error}});
				capability.pending_approval.reset();
				changed = true;
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
				(void)RetireCapabilityDirectory(app, capability);
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
						if (request.is_object() && request.contains("requestId") &&
						    request["requestId"].is_string() && request["requestId"] == request_id)
							response = HandleRequest(app, capability_id, request, now);
					}
				}
				(void)uam::paths::RemoveFileNoThrow(file);
				if (!response.value("pendingApproval", false) && !response.value("pendingAgentWait", false))
					(void)WriteResponse(capability_directory, request_id, response);
				changed = true;
			}
			++capability_index;
		}
		ProcessAgentWaits(app, response_write_hook);
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

	nlohmann::json UamControlService::ToolDefinitionsForTests()
	{
		return Tools();
	}
} // namespace uam
