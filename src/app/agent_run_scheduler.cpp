#include "app/agent_run_scheduler.h"

#include "app/agent_definition_service.h"
#include "app/agent_run_ledger.h"
#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <optional>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::size_t kMaxRunningAgents = 4;
		constexpr std::size_t kMaxQueuedAgents = 32;
		constexpr std::size_t kMaxDirectChildren = 4;
		constexpr std::size_t kMaxResultExcerptBytes = 16U * 1024U;
		constexpr int64_t kMaxRunElapsedMs = 30LL * 60LL * 1000LL;
		constexpr int64_t kProviderCrashWindowMs = 60LL * 1000LL;
		constexpr int64_t kProviderCircuitCooldownMs = 5LL * 60LL * 1000LL;
		constexpr std::size_t kProviderCrashThreshold = 3;

		int64_t SteadyMillisecondsNow()
		{
			return uam::time::SteadyEpochNanosecondsNow() / 1000000LL;
		}

		AgentRun* FindRun(AppState& app, std::string_view id)
		{
			const auto found = std::ranges::find(app.agent_runs, id, &AgentRun::id);
			return found == app.agent_runs.end() ? nullptr : &*found;
		}

		const AgentRun* FindRun(const AppState& app, std::string_view id)
		{
			const auto found = std::ranges::find(app.agent_runs, id, &AgentRun::id);
			return found == app.agent_runs.end() ? nullptr : &*found;
		}

		const AgentDefinition* FindAgent(const AgentDefinitionCatalog& catalog, std::string_view id)
		{
			const auto found = std::ranges::find(catalog.definitions, id, &AgentDefinition::id);
			return found == catalog.definitions.end() ? nullptr : &*found;
		}

		bool IsTerminal(std::string_view status)
		{
			return status == "completed" || status == "failed" || status == "cancelled" || status == "interrupted";
		}

		void RemoveRuntimeChat(AppState& app, std::string_view chat_id)
		{
			(void)StopAcpSession(app, std::string(chat_id));
			std::erase_if(app.acp_sessions, [&](const auto& session) { return session != nullptr && session->chat_id == chat_id; });
			std::erase_if(app.chats, [&](const ChatSession& chat) { return chat.id == chat_id; });
			app.chats_with_unseen_updates.erase(std::string(chat_id));
			app.resolved_native_sessions_by_chat_id.erase(std::string(chat_id));
		}

		std::string ResultExcerpt(const ChatSession& chat)
		{
			for (auto message = chat.messages.rbegin(); message != chat.messages.rend(); ++message)
			{
				if (message->role != MessageRole::Assistant) continue;
				std::string result = uam::strings::Trim(message->content);
				if (result.size() > kMaxResultExcerptBytes)
				{
					std::size_t end = kMaxResultExcerptBytes;
					while (end > 0 && (static_cast<unsigned char>(result[end]) & 0xc0U) == 0x80U) --end;
					result.resize(end);
				}
				return result;
			}
			return {};
		}

		bool PersistStatus(AppState& app, AgentRun& run, std::string status,
		                   std::string diagnostic_code = {}, std::string diagnostic = {})
		{
			const std::string now = uam::time::TimestampNow();
			run.status = std::move(status);
			run.updated_at = now;
			if (IsTerminal(run.status))
			{
				run.finished_at = now;
				if (run.result_delivery_id.empty()) run.result_delivery_id = run.id;
			}
			run.diagnostic_code = std::move(diagnostic_code);
			run.diagnostic = std::move(diagnostic);
			return AgentRunLedger::Save(app.data_root, run);
		}

		bool DeliverRootResult(AppState& app, AgentRun& run)
		{
			if (!run.deliver_result_to_root_chat || run.root_result_delivered ||
			    !IsTerminal(run.status) || run.root_result_delivery_attempts >= 3)
			{
				return false;
			}
			++run.root_result_delivery_attempts;
			ChatSession* root = ChatDomainService().FindChatById(app, run.root_chat_id);
			if (root == nullptr || !root->agent_run_id.empty())
			{
				run.root_result_delivery_attempts = 3;
				(void)AgentRunLedger::Save(app.data_root, run);
				return true;
			}

			const std::string tool_id = "uam-agent-result-" + run.id;
			const bool already_delivered = std::ranges::any_of(root->messages, [&](const Message& message)
			{
				return std::ranges::any_of(message.tool_calls, [&](const ToolCall& tool)
				{
					return tool.id == tool_id;
				});
			});
			if (!already_delivered)
			{
				const std::size_t previous_count = root->messages.size();
				const std::string previous_updated_at = root->updated_at;
				ChatDomainService().AddMessage(
				    *root, MessageRole::Assistant,
				    "Managed agent " + run.agent_id + " " + run.status + ".");
				ToolCall tool;
				tool.id = tool_id;
				tool.name = "uam_agent_result";
				tool.args_json = nlohmann::json{{"runId", run.id}}.dump();
				tool.result_text = nlohmann::json{
				    {"runId", run.id}, {"status", run.status}, {"agentId", run.agent_id},
				    {"providerId", run.provider_id}, {"executionCapability", run.execution_capability},
				    {"transcriptChatId", run.transcript_chat_id}, {"result", run.result_excerpt},
				    {"diagnosticCode", run.diagnostic_code}, {"diagnostic", run.diagnostic}}.dump();
				tool.status = run.status == "completed" ? "completed" : "failed";
				tool.is_sub_agent = true;
				tool.sub_agent_id = run.id;
				tool.sub_agent_title = run.agent_id;
				root->messages.back().tool_calls.push_back(std::move(tool));
				if (!ChatRepository::SaveChat(app.data_root, *root))
				{
					root->messages.resize(previous_count);
					root->updated_at = previous_updated_at;
					(void)AgentRunLedger::Save(app.data_root, run);
					return true;
				}
			}

			run.root_result_delivered = true;
			(void)AgentRunLedger::Save(app.data_root, run);
			return true;
		}

		void FinishRun(AppState& app, AgentRun& run, std::string status,
		               std::string diagnostic_code = {}, std::string diagnostic = {})
		{
			if (ChatSession* chat = ChatDomainService().FindChatById(app, run.transcript_chat_id); chat != nullptr)
			{
				run.result_excerpt = ResultExcerpt(*chat);
			}
			if (!PersistStatus(app, run, std::move(status), std::move(diagnostic_code), std::move(diagnostic)))
			{
				run.status = "failed";
				run.diagnostic_code = "persistence_failed";
				run.diagnostic = "The final agent-run state could not be persisted.";
				(void)AgentRunLedger::Save(app.data_root, run);
			}
			(void)DeliverRootResult(app, run);
			app.agent_run_deadline_steady_ms.erase(run.id);
			RemoveRuntimeChat(app, run.transcript_chat_id);
		}

		bool ProviderExists(const AppState& app, std::string_view provider_id)
		{
			const std::string normalized = provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
			return std::ranges::any_of(app.provider_profiles, [&](const ProviderProfile& provider)
			{
				return provider_ids::NormalizeCliProviderAliasOrSelf(provider.id) == normalized;
			});
		}

		std::string ProviderKey(std::string_view provider_id)
		{
			return provider_ids::NormalizeCliProviderAliasOrSelf(provider_id);
		}

		void RecordProviderCrash(AppState& app, std::string_view provider_id, int64_t now_epoch_ms)
		{
			const std::string key = ProviderKey(provider_id);
			auto& crashes = app.agent_provider_crash_times_epoch_ms[key];
			while (!crashes.empty() && crashes.front() <= now_epoch_ms - kProviderCrashWindowMs) crashes.pop_front();
			crashes.push_back(now_epoch_ms);
			if (crashes.size() >= kProviderCrashThreshold)
			{
				app.agent_provider_circuit_open_until_epoch_ms[key] = now_epoch_ms + kProviderCircuitCooldownMs;
			}
		}

		bool ProviderCircuitOpen(AppState& app, std::string_view provider_id, int64_t now_epoch_ms)
		{
			const std::string key = ProviderKey(provider_id);
			const auto found = app.agent_provider_circuit_open_until_epoch_ms.find(key);
			if (found == app.agent_provider_circuit_open_until_epoch_ms.end()) return false;
			if (now_epoch_ms < found->second) return true;
			app.agent_provider_circuit_open_until_epoch_ms.erase(found);
			app.agent_provider_crash_times_epoch_ms.erase(key);
			return false;
		}
	} // namespace

	bool AgentRunScheduler::Enqueue(AppState& app, const std::string& root_chat_id,
	                                const std::string& parent_run_id, const std::string& agent_id,
	                                const std::string& task, std::string* run_id_out,
	                                std::string* error_out, const std::string& provider_id,
	                                const std::string& model_id)
	{
		ChatSession* root_chat = ChatDomainService().FindChatById(app, uam::strings::Trim(root_chat_id));
		const std::string requested_agent = uam::strings::Trim(agent_id);
		const std::string requested_task = uam::strings::Trim(task);
		if (root_chat == nullptr || !root_chat->agent_run_id.empty() || requested_agent.empty() || requested_task.empty())
		{
			if (error_out != nullptr) *error_out = "Delegation requires a visible root chat, agent, and task.";
			return false;
		}
		if (root_chat->imported_read_only)
		{
			if (error_out != nullptr) *error_out = "Imported transcripts are read-only. Create a new chat in a workspace to delegate work.";
			return false;
		}
		const std::size_t queued_count = std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status);
		if (queued_count >= kMaxQueuedAgents)
		{
			if (error_out != nullptr) *error_out = "The bounded 32-run agent queue is full.";
			return false;
		}

		const AgentRun* parent = parent_run_id.empty() ? nullptr : FindRun(app, parent_run_id);
		if (!parent_run_id.empty() && (parent == nullptr || parent->status != "running" || parent->root_chat_id != root_chat->id))
		{
			if (error_out != nullptr) *error_out = "The parent agent run is missing, settled, or belongs to another chat.";
			return false;
		}
		const int depth = parent == nullptr ? 1 : parent->depth + 1;
		if (depth > 2)
		{
			if (error_out != nullptr) *error_out = "Agent delegation is limited to two levels.";
			return false;
		}
		if (parent != nullptr && std::ranges::count(app.agent_runs, parent->id, &AgentRun::parent_run_id) >= kMaxDirectChildren)
		{
			if (error_out != nullptr) *error_out = "An agent run may create at most four direct children.";
			return false;
		}
		for (const AgentRun* ancestor = parent; ancestor != nullptr; ancestor = ancestor->parent_run_id.empty() ? nullptr : FindRun(app, ancestor->parent_run_id))
		{
			if (ancestor->agent_id == requested_agent)
			{
				if (error_out != nullptr) *error_out = "Self-delegation and agent cycles are not allowed.";
				return false;
			}
		}

		const AgentDefinitionCatalog agents = AgentDefinitionService::Load(app.data_root, uam::paths::ResolveWorkspaceRootPath(app, *root_chat));
		const AgentDefinition* definition = FindAgent(agents, requested_agent);
		if (definition == nullptr || (definition->mode != "subagent" && definition->mode != "both"))
		{
			if (error_out != nullptr) *error_out = "Requested agent is unavailable for delegation: " + requested_agent + ".";
			return false;
		}
		if (parent != nullptr)
		{
			if (!uam::ranges::Contains(parent->delegates_snapshot, requested_agent))
			{
				if (error_out != nullptr) *error_out = "The parent run does not authorise this delegate under its saved definition.";
				return false;
			}
		}

		const std::string selected_provider = uam::strings::NonEmptyOrFallback(
		    uam::strings::Trim(provider_id), root_chat->provider_id);
		if (!ProviderExists(app, selected_provider))
		{
			if (error_out != nullptr) *error_out = "Requested provider is unavailable.";
			return false;
		}
		const AcpSessionState* root_session = FindAcpSessionForChat(app, root_chat->id);
		const AgentDefinition* root_definition = FindAgent(agents, root_chat->uam_agent_id);
		const bool root_agent_read_only = root_session != nullptr
		                                      ? root_session->active_uam_agent_workspace_access == "read"
		                                      : root_definition != nullptr && root_definition->workspace_access == "read";
		const bool read_only = definition->workspace_access == "read" || root_agent_read_only ||
		                       approval_modes::AppApprovalModeOrEmpty(root_chat->approval_mode) == approval_modes::kPlanApprovalMode ||
		                       (parent != nullptr && parent->effective_workspace_access == "read");

		ChatSession transcript = ChatDomainService().CreateNewChat(root_chat->folder_id, selected_provider);
		transcript.title = "Agent: " + requested_agent;
		transcript.workspace_directory = root_chat->workspace_directory;
		transcript.model_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(model_id), root_chat->model_id);
		transcript.reasoning_effort = root_chat->reasoning_effort;
		transcript.service_tier = root_chat->service_tier;
		transcript.service_tier_explicit = root_chat->service_tier_explicit;
		transcript.command_safety_tier = root_chat->command_safety_tier;
		transcript.approval_mode = read_only ? approval_modes::kPlanApprovalMode : root_chat->approval_mode;
		transcript.uam_agent_id = requested_agent;
		transcript.uam_control_enabled = root_chat->uam_control_enabled;

		AgentRun run;
		run.id = AgentRunLedger::NewRunId();
		run.root_chat_id = root_chat->id;
		run.parent_run_id = parent == nullptr ? std::string{} : parent->id;
		run.transcript_chat_id = transcript.id;
		run.goal_id = root_chat->active_goal_id;
		run.agent_id = definition->id;
		run.definition_hash = definition->definition_hash;
		run.definition_snapshot = definition->markdown_snapshot;
		run.definition_instructions = definition->instructions;
		run.skills_snapshot = definition->skills;
		run.delegates_snapshot = definition->delegates;
		run.provider_id = selected_provider;
		run.model_id = transcript.model_id;
		run.execution_capability =
		    AgentDefinitionService::ExecutionCapabilityForProvider(selected_provider);
		run.task = requested_task;
		run.effective_workspace_access = read_only ? "read" : "write";
		run.status = "queued";
		run.depth = depth;
		run.created_at = uam::time::TimestampNow();
		run.updated_at = run.created_at;
		run.result_delivery_id = run.id;
		transcript.agent_run_id = run.id;

		std::string save_error;
		if (!AgentRunLedger::Save(app.data_root, run, &save_error))
		{
			if (error_out != nullptr) *error_out = save_error;
			return false;
		}
		if (!ChatRepository::SaveChat(app.data_root, transcript))
		{
			(void)PersistStatus(app, run, "failed", "transcript_save_failed", "The managed transcript could not be persisted.");
			if (error_out != nullptr) *error_out = "The managed transcript could not be persisted.";
			return false;
		}

		if (run_id_out != nullptr) *run_id_out = run.id;
		app.agent_runs.push_back(run);
		app.chats.push_back(std::move(transcript));
		app.queued_agent_run_ids.push_back(run.id);
		return true;
	}

	bool AgentRunScheduler::TryEnqueueMention(AppState& app, const std::string& root_chat_id,
	                                          const std::string& text, bool* handled_out,
	                                          std::string* run_id_out, std::string* error_out)
	{
		if (handled_out != nullptr) *handled_out = false;
		if (text.size() < 3 || text.front() != '@' || text[1] == '@') return true;
		std::size_t end = 1;
		while (end < text.size())
		{
			const unsigned char ch = static_cast<unsigned char>(text[end]);
			if (!(std::islower(ch) || std::isdigit(ch) || ch == '-')) break;
			++end;
		}
		if (end == 1 || end > 65 || end >= text.size() ||
		    !std::isspace(static_cast<unsigned char>(text[end])))
		{
			return true;
		}

		ChatSession* root_chat = ChatDomainService().FindChatById(app, root_chat_id);
		if (root_chat == nullptr || !root_chat->agent_run_id.empty()) return true;
		const std::string agent_id = text.substr(1, end - 1);
		const AgentDefinitionCatalog catalog = AgentDefinitionService::Load(
		    app.data_root, uam::paths::ResolveWorkspaceRootPath(app, *root_chat));
		const AgentDefinition* definition = FindAgent(catalog, agent_id);
		if (definition == nullptr || (definition->mode != "subagent" && definition->mode != "both"))
		{
			return true;
		}
		if (handled_out != nullptr) *handled_out = true;
		const std::string task = uam::strings::Trim(text.substr(end));
		if (task.empty())
		{
			if (error_out != nullptr) *error_out = "Add a task after @" + agent_id + ".";
			return false;
		}

		std::string run_id;
		if (!Enqueue(app, root_chat->id, {}, agent_id, task, &run_id, error_out)) return false;
		AgentRun* run = FindRun(app, run_id);
		if (run == nullptr)
		{
			if (error_out != nullptr) *error_out = "The @agent run could not be found after it was queued.";
			return false;
		}
		run->deliver_result_to_root_chat = true;
		if (!AgentRunLedger::Save(app.data_root, *run))
		{
			run->deliver_result_to_root_chat = false;
			std::string cancel_error;
			(void)CancelTree(app, run_id, &cancel_error);
			if (error_out != nullptr) *error_out = "The @agent delivery contract could not be persisted, so the run was cancelled.";
			return false;
		}
		root_chat = ChatDomainService().FindChatById(app, root_chat_id);
		if (root_chat == nullptr)
		{
			run->deliver_result_to_root_chat = false;
			(void)AgentRunLedger::Save(app.data_root, *run);
			std::string cancel_error;
			(void)CancelTree(app, run_id, &cancel_error);
			if (error_out != nullptr) *error_out = "The @agent parent chat disappeared, so the run was cancelled.";
			return false;
		}
		const std::size_t previous_message_count = root_chat->messages.size();
		const std::string previous_updated_at = root_chat->updated_at;
		ChatDomainService().AddMessage(*root_chat, MessageRole::User, text);
		if (!ChatRepository::SaveChat(app.data_root, *root_chat))
		{
			root_chat->messages.resize(previous_message_count);
			root_chat->updated_at = previous_updated_at;
			run->deliver_result_to_root_chat = false;
			(void)AgentRunLedger::Save(app.data_root, *run);
			std::string cancel_error;
			(void)CancelTree(app, run_id, &cancel_error);
			if (error_out != nullptr) *error_out = "The @agent request could not be persisted, so it was cancelled.";
			return false;
		}
		if (run_id_out != nullptr) *run_id_out = run_id;
		return true;
	}

	bool AgentRunScheduler::ResumeInterrupted(AppState& app, const std::string& run_id,
	                                          std::string* new_run_id_out,
	                                          std::string* error_out)
	{
		AgentRun* interrupted = FindRun(app, uam::strings::Trim(run_id));
		if (interrupted == nullptr || interrupted->status != "interrupted")
		{
			if (error_out != nullptr) *error_out = "Only an interrupted managed run can be resumed as a fresh run.";
			return false;
		}
		ChatSession* root_chat = ChatDomainService().FindChatById(app, interrupted->root_chat_id);
		if (root_chat == nullptr || !root_chat->agent_run_id.empty() ||
		    !ProviderExists(app, interrupted->provider_id))
		{
			if (error_out != nullptr) *error_out = "The interrupted run's visible chat or provider is unavailable.";
			return false;
		}
		if (std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status) >= kMaxQueuedAgents)
		{
			if (error_out != nullptr) *error_out = "The bounded 32-run agent queue is full.";
			return false;
		}
		if (std::ranges::any_of(app.agent_runs, [&](const AgentRun& candidate)
		    {
			return candidate.resumed_from_run_id == interrupted->id && !IsTerminal(candidate.status);
		}))
		{
			if (error_out != nullptr) *error_out = "A fresh continuation of this interrupted run is already active.";
			return false;
		}

		std::optional<ChatSession> loaded_transcript;
		ChatSession* old_transcript = ChatDomainService().FindChatById(app, interrupted->transcript_chat_id);
		if (old_transcript == nullptr)
		{
			std::string warning;
			loaded_transcript = ChatRepository::LoadLocalChat(
			    app.data_root, interrupted->transcript_chat_id, true, &warning);
			if (!loaded_transcript.has_value())
			{
				if (error_out != nullptr) *error_out = uam::strings::NonEmptyOrFallback(
				    warning, "The interrupted run's durable transcript is unavailable.");
				return false;
			}
			old_transcript = &*loaded_transcript;
		}
		if (old_transcript->agent_run_id != interrupted->id)
		{
			if (error_out != nullptr) *error_out = "The durable transcript does not belong to the interrupted run.";
			return false;
		}

		const ChatSession fresh_identity = ChatDomainService().CreateNewChat(
		    old_transcript->folder_id, interrupted->provider_id);
		ChatSession transcript = *old_transcript;
		transcript.id = fresh_identity.id;
		transcript.native_session_id.clear();
		transcript.parent_chat_id.clear();
		transcript.branch_root_chat_id.clear();
		transcript.branch_from_message_index = -1;
		transcript.branch_message_edited = false;
		transcript.title = "Agent: " + interrupted->agent_id + " (resumed)";
		transcript.created_at = fresh_identity.created_at;
		transcript.updated_at = fresh_identity.updated_at;
		transcript.last_opened_at.clear();
		transcript.pinned = false;
		transcript.persisted_message_count = 0;
		transcript.persisted_messages_digest.clear();

		AgentRun run = *interrupted;
		run.id = AgentRunLedger::NewRunId();
		run.parent_run_id.clear();
		run.resumed_from_run_id = interrupted->id;
		run.transcript_chat_id = transcript.id;
		run.execution_capability =
		    AgentDefinitionService::ExecutionCapabilityForProvider(interrupted->provider_id);
		run.status = "queued";
		run.depth = 1;
		run.expected_turn_serial = 0;
		run.started_at_epoch_ms = 0;
		run.deadline_at_epoch_ms = 0;
		run.created_at = uam::time::TimestampNow();
		run.started_at.clear();
		run.finished_at.clear();
		run.updated_at = run.created_at;
		run.result_delivery_id = run.id;
		run.root_result_delivered = false;
		run.root_result_delivery_attempts = 0;
		run.result_excerpt.clear();
		run.diagnostic_code.clear();
		run.diagnostic.clear();
		run.task = "Continue the interrupted managed task from the durable transcript. "
		           "Complete only the remaining work and do not repeat work already shown there.";
		transcript.agent_run_id = run.id;

		std::string save_error;
		if (!AgentRunLedger::Save(app.data_root, run, &save_error))
		{
			if (error_out != nullptr) *error_out = save_error;
			return false;
		}
		if (!ChatRepository::SaveChat(app.data_root, transcript))
		{
			(void)PersistStatus(app, run, "failed", "transcript_save_failed",
			                    "The fresh continuation transcript could not be persisted.");
			if (error_out != nullptr) *error_out = "The fresh continuation transcript could not be persisted.";
			return false;
		}
		app.agent_runs.push_back(run);
		app.chats.push_back(std::move(transcript));
		app.queued_agent_run_ids.push_back(run.id);
		if (new_run_id_out != nullptr) *new_run_id_out = run.id;
		return true;
	}

	static bool PollAt(AppState& app, int64_t now_epoch_ms, int64_t now_steady_ms)
	{
		bool changed = false;
		for (AgentRun& run : app.agent_runs)
		{
			if (DeliverRootResult(app, run)) changed = true;
		}
		for (AgentRun& run : app.agent_runs)
		{
			if (run.status != "running") continue;
			AcpSessionState* session = FindAcpSessionForChat(app, run.transcript_chat_id);
			if (session != nullptr && run.expected_turn_serial > 0 &&
			    session->last_settled_turn_serial >= run.expected_turn_serial)
			{
				FinishRun(app, run, session->last_turn_outcome == "completed" ? "completed" : "failed",
				          session->last_turn_outcome == "completed" ? "" : "provider_turn_failed",
				          session->last_turn_error);
				changed = true;
				continue;
			}
			const auto steady_deadline = app.agent_run_deadline_steady_ms.find(run.id);
			const bool deadline_expired = steady_deadline != app.agent_run_deadline_steady_ms.end()
			                                  ? now_steady_ms >= steady_deadline->second
			                                  : run.deadline_at_epoch_ms > 0 && now_epoch_ms >= run.deadline_at_epoch_ms;
			if (deadline_expired)
			{
				FinishRun(app, run, "failed", "wall_clock_deadline_exceeded",
				          "The managed agent run exceeded its 30-minute total elapsed-time limit.");
				changed = true;
				continue;
			}
			if (session != nullptr && session->managed_launch_attempted && !session->running &&
			    !session->processing && !session->reconnect_pending)
			{
				RecordProviderCrash(app, run.provider_id, now_epoch_ms);
				FinishRun(app, run, "failed", "provider_exited",
				          uam::strings::NonEmptyOrFallback(session->last_error, "The provider exited before the managed turn settled."));
				changed = true;
			}
		}

		std::size_t running = std::ranges::count(app.agent_runs, std::string("running"), &AgentRun::status);
		while (running < kMaxRunningAgents && !app.queued_agent_run_ids.empty())
		{
			const std::string id = std::move(app.queued_agent_run_ids.front());
			app.queued_agent_run_ids.pop_front();
			AgentRun* run = FindRun(app, id);
			if (run == nullptr || run->status != "queued") continue;
			ChatSession* transcript = ChatDomainService().FindChatById(app, run->transcript_chat_id);
			if (transcript == nullptr)
			{
				(void)PersistStatus(app, *run, "failed", "transcript_missing", "The managed transcript is missing.");
				changed = true;
				continue;
			}
			if (ProviderCircuitOpen(app, run->provider_id, now_epoch_ms))
			{
				FinishRun(app, *run, "failed", "provider_crash_circuit_open",
				          "The provider crashed three times within one minute; managed launches are paused for five minutes.");
				changed = true;
				continue;
			}

			run->status = "running";
			run->started_at = uam::time::TimestampNow();
			run->updated_at = run->started_at;
			run->started_at_epoch_ms = now_epoch_ms;
			run->deadline_at_epoch_ms = now_epoch_ms + kMaxRunElapsedMs;
			app.agent_run_deadline_steady_ms[run->id] = now_steady_ms + kMaxRunElapsedMs;
			if (!AgentRunLedger::Save(app.data_root, *run))
			{
				run->status = "failed";
				FinishRun(app, *run, "failed", "running_state_save_failed", "The running state could not be persisted, so UAM did not launch the provider.");
				changed = true;
				continue;
			}

			std::string launch_error;
			if (!SendAcpPrompt(app, transcript->id, run->task, {}, {}, false, &launch_error))
			{
				FinishRun(app, *run, "failed", "launch_failed", launch_error);
				changed = true;
				continue;
			}
			if (AcpSessionState* session = FindAcpSessionForChat(app, transcript->id); session != nullptr)
			{
				run->expected_turn_serial = std::max(1, session->turn_serial);
			}
			if (!AgentRunLedger::Save(app.data_root, *run))
			{
				FinishRun(app, *run, "failed", "launch_record_save_failed", "The launch record could not be persisted; UAM stopped the provider.");
				changed = true;
				continue;
			}
			++running;
			changed = true;
		}
		return changed;
	}

	bool AgentRunScheduler::Poll(AppState& app)
	{
		return PollAt(app, uam::time::SystemEpochMillisecondsNow(), SteadyMillisecondsNow());
	}

	bool AgentRunScheduler::PollAtForTests(AppState& app, int64_t now_epoch_ms)
	{
		return PollAt(app, now_epoch_ms, now_epoch_ms);
	}

	bool AgentRunScheduler::GetResultForParent(const AppState& app, const std::string& run_id,
	                                          const std::string& root_chat_id,
	                                          const std::string& parent_run_id,
	                                          AgentRunResult* result_out,
	                                          std::string* error_out)
	{
		const AgentRun* run = FindRun(app, uam::strings::Trim(run_id));
		if (run == nullptr || run->root_chat_id != uam::strings::Trim(root_chat_id) ||
		    run->parent_run_id != uam::strings::Trim(parent_run_id))
		{
			if (error_out != nullptr) *error_out = "Agent run is outside the requesting parent scope.";
			return false;
		}
		if (!IsTerminal(run->status))
		{
			if (error_out != nullptr) *error_out = "Agent run has not produced a terminal result.";
			return false;
		}
		if (result_out != nullptr)
		{
			result_out->delivery_id = uam::strings::NonEmptyOrFallback(run->result_delivery_id, run->id);
			result_out->run_id = run->id;
			result_out->status = run->status;
			result_out->agent_id = run->agent_id;
			result_out->output = run->result_excerpt;
			result_out->diagnostic_code = run->diagnostic_code;
			result_out->diagnostic = run->diagnostic;
		}
		return true;
	}

	bool AgentRunScheduler::CancelTree(AppState& app, const std::string& run_id,
	                                   std::string* error_out)
	{
		if (FindRun(app, run_id) == nullptr)
		{
			if (error_out != nullptr) *error_out = "Agent run not found.";
			return false;
		}
		std::unordered_set<std::string> tree{run_id};
		bool expanded = true;
		while (expanded)
		{
			expanded = false;
			for (const AgentRun& run : app.agent_runs)
			{
				if (!run.parent_run_id.empty() && tree.contains(run.parent_run_id) && tree.insert(run.id).second) expanded = true;
			}
		}
		std::vector<AgentRun*> runs;
		for (AgentRun& run : app.agent_runs) if (tree.contains(run.id) && !IsTerminal(run.status)) runs.push_back(&run);
		std::ranges::sort(runs, std::greater{}, &AgentRun::depth);
		bool success = true;
		for (AgentRun* run : runs)
		{
			if (!PersistStatus(app, *run, "cancelled", "user_cancelled", "The user cancelled this agent run.")) success = false;
			app.agent_run_deadline_steady_ms.erase(run->id);
			RemoveRuntimeChat(app, run->transcript_chat_id);
		}
		std::erase_if(app.queued_agent_run_ids, [&](const std::string& id) { return tree.contains(id); });
		if (!success && error_out != nullptr) *error_out = "One or more cancellation records could not be persisted; all provider processes were still stopped.";
		return success;
	}

	bool AgentRunScheduler::InterruptForShutdown(AppState& app)
	{
		std::vector<std::string> errors;
		const bool interrupted = AgentRunLedger::MarkNonterminalInterrupted(
		    app.data_root, &app.agent_runs, "manager_shutdown", &errors);
		app.agent_run_deadline_steady_ms.clear();
		return interrupted;
	}
} // namespace uam
