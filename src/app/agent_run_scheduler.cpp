#include "app/managed_parent_continuation_service.h"
#include "app/execution_routing_integration.h"
#include "app/agent_run_scheduler.h"
#include "app/managed_subagent_followup_service.h"

#include "app/agent_definition_service.h"
#include "app/agent_run_ledger.h"
#include "app/provider_resolution_service.h"
#include "common/config/execution_host_config.h"
#include "common/config/host_scoped_settings.h"
#include "common/config/provider_availability.h"
#include "common/config/provider_chat_defaults.h"
#include "common/memory/memory_policy.h"
#include "common/config/settings_frontend_json.h"
#include "common/platform/platform_services.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_model_json.h"
#include "common/runtime/native_refresh_lifetime.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/utils/uuid.h"
#include "common/utils/io_utils.h"
#include "common/paths/app_paths.h"
#include "common/platform/platform_incremental_sha256.h"

#include "app/managed_subagent_control_service.h"
#include "app/chat_domain_service.h"
#include "app/runtime_orchestration_services.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/mcp_server_config.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/diagnostic_log.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <atomic>
#include <chrono>
#include <thread>
#include <tuple>
#include <cctype>
#include <deque>
#include <optional>
#include <unordered_set>

namespace uam
{
	namespace
	{
		using InitialClock = std::chrono::steady_clock;
		struct InitialPreparation
		{
			std::filesystem::path root;
			ChatSession before, parent, transcript;
			AgentRun run;
			std::shared_ptr<PreparedChatPublication> root_prepared, transcript_prepared;
			std::shared_ptr<PreparedAgentRunSave> ledger_prepared;
			std::atomic<bool> finished{false}, cancel{false};
			bool root_before_observed = false, root_was_absent = false;
			std::shared_ptr<std::atomic<bool>> retired;
			std::string error;
			~InitialPreparation()
			{
				root_prepared.reset();
				transcript_prepared.reset();
				ledger_prepared.reset();
				before = {};
				parent = {};
				transcript = {};
				run = {};
				if (retired)
					retired->store(true, std::memory_order_release);
			}
		};
		struct InitialTransition
		{
			AgentRun expected, candidate;
			ChatSession binding_transcript;
			std::string binding_primary_revision, binding_primary_digest, binding_root_revision, binding_root_created;
			std::uint64_t binding_generation = 0, binding_save_revision = 0;
			bool binding_requested = false;
			bool resume_terminal = false;
			std::shared_ptr<PreparedAgentRunSave> prepared;
			std::optional<AgentRun> observed;
			std::shared_ptr<PreparedChatPublication> root_prepared;
			ChatSession parent_before, parent_candidate;
			std::string parent_metadata, cold_metadata, error;
			std::shared_ptr<ChatDomainService::ColdHistoryPreparation> cold;
			bool cold_applied = false;
			std::atomic<bool> finished{false};
			std::shared_ptr<std::atomic<bool>> retired;
			~InitialTransition()
			{
				prepared.reset();
				root_prepared.reset();
				cold.reset();
				expected = {};
				candidate = {};
				binding_transcript = {};
				observed.reset();
				parent_before = {};
				parent_candidate = {};
				if (retired)
					retired->store(true, std::memory_order_release);
			}
		};
		struct FollowupPreparation
		{
			std::filesystem::path root;
			AgentRun expected, candidate;
			ChatSession parent, transcript;
			std::optional<ChatSession> live_transcript;
			std::shared_ptr<PreparedAgentRunSave> prepared;
			std::optional<AgentRun> observed;
			std::string root_revision, transcript_revision, predecessor_revision, predecessor_digest, ledger_stamp, error;
			ManagedFollowupCode code = ManagedFollowupCode::PersistenceFailed;
			std::atomic<bool> finished{false}, cancel{false};
			std::shared_ptr<std::atomic<bool>> retired;
			~FollowupPreparation()
			{
				prepared.reset();
				expected = {}; candidate = {}; parent = {}; transcript = {}; live_transcript.reset(); observed.reset();
				if (retired) retired->store(true, std::memory_order_release);
			}
		};
	} // namespace

	struct PendingManagedInitialMention
	{
		enum class Phase
		{
			PreparingRoot,
			ReconcilingRoot,
			Cleaning,
			Callback,
			UncertainShutdown
		};
		Phase phase = Phase::PreparingRoot;
		std::filesystem::path root;
		std::string root_id, transcript_id, root_created, root_birth, metadata, settings;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::shared_ptr<InitialPreparation> work;
		std::shared_ptr<std::atomic<bool>> retired = std::make_shared<std::atomic<bool>>(false);
		std::function<bool(const ManagedInitialMentionResult&)> completion;
		std::function<bool(ChatPublicationFile, io::AtomicWriteStage)> root_stage_hook_for_tests;
		std::function<bool(ChatPublicationFile, io::AtomicWriteStage)> parent_stage_hook_for_tests;
		ManagedInitialMentionResult result;
		std::string accepted_run_id, accepted_transcript_birth, accepted_transcript_created;
		std::shared_ptr<const std::uint8_t> accepted_transcript_incarnation;
		InitialClock::time_point deadline = InitialClock::now() + std::chrono::seconds(60);
		bool cancelled = false, accepted = false, completion_settled = false, uncertain_root = false, confirmation_preparing = false;
		InitialClock::time_point retry_at{};
	};

	struct ManagedFollowupCompletedOwnerWitness
	{
		std::filesystem::path root;
		std::string root_id, run_id, transcript_id, root_created, run_created, provider_id, agent_id, definition_hash, workspace_access, turn_id;
		std::shared_ptr<const std::uint8_t> root_incarnation;
		ManagedRunOwnerBinding owner;
		ManagedRunExecutionBinding execution;
		std::uint64_t state_revision = 0;
		std::string primary_revision, primary_digest;
		InitialClock::time_point retained_at = InitialClock::now();
		std::size_t retained_bytes = 0;
		ChatSession metadata;
		std::string dispatch_settings, child_policy;
	};

	struct PendingManagedFollowup
	{
		enum class Phase { Preparing, Cleaning, Uncertain };
		Phase phase = Phase::Preparing;
		std::filesystem::path root;
		std::string root_id, run_id, transcript_id, root_created, root_birth, metadata, settings;
		std::shared_ptr<const std::uint8_t> incarnation;
		std::shared_ptr<FollowupPreparation> work;
		std::shared_ptr<std::atomic<bool>> retired = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<const ManagedFollowupCompletedOwnerWitness> source;
		ManagedFollowupRequest request;
		ManagedFollowupSubmitResult result;
		std::function<bool(const ManagedFollowupSubmitResult&)> completion;
		InitialClock::time_point deadline = InitialClock::now() + std::chrono::seconds(60), retry_at{};
		bool replay = false, cancelled = false, accepted = false, completion_settled = false, reconciling = false;
	};

	struct ManagedInitialRuntimeOwner
	{
		enum class Phase
		{
			Queued,
			Starting,
			Row,
			Input,
			Stopping,
			Result,
			Done
		};
		enum class Action
		{
			Running,
			Row,
			Terminal,
			Attempts,
			Delivered,
			Reconcile,
			Parent
		};
		Phase phase = Phase::Queued;
		Action action = Action::Running;
		std::filesystem::path root;
		std::string root_id, transcript_id, root_birth, transcript_birth, root_created, transcript_created;
		std::shared_ptr<const std::uint8_t> root_incarnation, transcript_incarnation;
		std::string settings, child_policy, root_policy, root_settings, process, native_observed, saved_native, wire_policy;
		std::string desired_status, diagnostic_code, diagnostic, output, pending_parent_metadata;
		std::optional<ManagedRunExecutionBinding> dispatch_configuration;
		std::string dispatch_settings, observed_native, observed_model, observed_mode, observed_workspace, observed_provider_version;
		bool setup_execution_observed = false;
		std::string completed_primary_revision, completed_primary_digest;
		std::uint64_t observation_generation = 0, saved_observation_generation = 0, saved_observation_revision = 0;
		bool configuration_captured = false, binding_withheld = false;
		bool resume_only = false, terminal_committed = false;
		AgentRun expected;
		std::optional<AgentRun> uncertain_candidate;
		const AcpSessionState* session = nullptr;
		std::shared_ptr<InitialTransition> transition;
		std::shared_ptr<std::atomic<bool>> retired;
		InitialClock::time_point retry_at{};
		bool completion_acked = false, send_authorized = false, send_called = false, process_attempted = false;
		bool input_forbidden = false, input_attempted = false, input_written = false, settled = false, parent_result_acked = false, native_response_seen = false;
		int serial = 0, row = -1, saved_serial = 0, saved_row = -1;
		std::string row_created, saved_row_created;
		std::shared_ptr<platform::ObservedProcessStopResult> stop_result;
		std::shared_ptr<std::atomic<bool>> stop_finished;
		bool orphan_stop_transferred = false, parent_result_uncertain = false, parent_retry_prepared = false;
		std::function<bool(ChatPublicationFile, io::AtomicWriteStage)> parent_stage_hook_for_tests;
		bool parent_stage_hook_used = false;
	};

	namespace
	{
		const ManagedRunTurnReceipt& InitialCurrentTurn(const ManagedInitialRuntimeOwner& owner)
		{
			return owner.expected.managed_continuation->turns.back();
		}
		const std::string& InitialTask(const ManagedInitialRuntimeOwner& owner)
		{
			return owner.resume_only ? InitialCurrentTurn(owner).text : owner.expected.task;
		}
		const std::string& InitialDeliveryId(const ManagedInitialRuntimeOwner& owner)
		{
			return owner.resume_only ? InitialCurrentTurn(owner).delivery_id : owner.expected.id;
		}
		bool InitialResultDelivered(const ManagedInitialRuntimeOwner& owner)
		{
			return owner.resume_only ? InitialCurrentTurn(owner).result_delivered : owner.expected.root_result_delivered;
		}
		int InitialResultAttempts(const ManagedInitialRuntimeOwner& owner)
		{
			return owner.resume_only ? InitialCurrentTurn(owner).result_delivery_attempts : owner.expected.root_result_delivery_attempts;
		}
		bool FollowupTaskScope(const AppState&, const PendingManagedFollowup&);
		bool PollFollowupCreations(AppState&);
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

		std::size_t RunningAgentSlots(const AppState& app)
		{
			std::size_t running = std::ranges::count(app.agent_runs, std::string("running"), &AgentRun::status);
			for (const auto& [id, owner] : app.managed_initial_runtime_owners)
			{
				const bool reserved = owner && ((owner->transition && owner->action == ManagedInitialRuntimeOwner::Action::Running) || (owner->uncertain_candidate && owner->uncertain_candidate->status == "running"));
				if (reserved && std::ranges::none_of(app.agent_runs, [&](const AgentRun& run) { return run.id == id && run.status == "running"; }))
					++running;
			}
			return running;
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

		void EraseRuntimeChat(AppState& app, std::string_view chat_id)
		{
			if (const ChatSession* chat = ChatDomainService().FindChatById(app, std::string(chat_id)))
				ChatHistorySyncService().RetainHiddenAgentChatOwnership(app, *chat);
			std::erase_if(app.acp_sessions, [&](const auto& session) { return session != nullptr && session->chat_id == chat_id; });
			std::erase_if(app.chats, [&](const ChatSession& chat) { return chat.id == chat_id; });
			app.chats_with_unseen_updates.erase(std::string(chat_id));
			app.resolved_native_sessions_by_chat_id.erase(std::string(chat_id));
		}

		bool RemoveRuntimeChat(AppState& app, std::string_view chat_id)
		{
			if (!StopAcpSession(app, std::string(chat_id))) return false;
			EraseRuntimeChat(app, chat_id);
			return true;
		}

		std::string ResultExcerpt(const ChatSession& chat, std::size_t first = 0)
		{
			for (auto message = chat.messages.rbegin(); message != chat.messages.rend(); ++message)
			{
				if (static_cast<std::size_t>(std::distance(message, chat.messages.rend()) - 1) < first) break;
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
			const int64_t now_epoch_ms = uam::time::SystemEpochMillisecondsNow();
			const std::string now = uam::time::detail::FormatTimestamp(uam::time::detail::LocalTimeSnapshot(static_cast<std::time_t>(now_epoch_ms / 1000)), uam::time::kLocalDisplayTimestampFormat);
			AgentRun updated = run;
			updated.status = std::move(status);
			updated.updated_at = now;
			if (IsTerminal(updated.status))
			{
				updated.finished_at = now;
				updated.finished_at_epoch_ms = now_epoch_ms;
				if (updated.result_delivery_id.empty()) updated.result_delivery_id = updated.id;
			}
			updated.diagnostic_code = std::move(diagnostic_code);
			updated.diagnostic = std::move(diagnostic);
			if (!AgentRunLedger::Save(app.data_root, updated)) return false;
			run = std::move(updated);
			if (ChatSession* root = ChatDomainService().FindChatById(app, run.root_chat_id))
				++root->managed_subagent_revision;
			return true;
		}

		bool InitialStoredChatMatches(const std::filesystem::path& root, ChatSession saved, const ChatSession& captured);

		bool DeliverRootResult(AppState& app, AgentRun& run)
		{
			if (!run.deliver_result_to_root_chat || run.root_result_delivered ||
			    !IsTerminal(run.status) || run.root_result_delivery_attempts >= 3)
			{
				return false;
			}
			ChatSession* root = ChatDomainService().FindChatById(app, run.root_chat_id);
			if (root == nullptr || !root->agent_run_id.empty())
			{
				run.root_result_delivery_attempts = 3;
				(void)AgentRunLedger::Save(app.data_root, run);
				return true;
			}

			std::string warning;
			if (!ChatRepository::HydrateChatMessages(app.data_root, *root, &warning))
			{
				diagnostics::Write("agent result: could not load parent chat " + root->id + ": " + warning);
				(void)AgentRunLedger::Save(app.data_root, run);
				return true;
			}
			// A loaded parent is not proof that its saved history is still current.
			// Refuse stale data rather than replacing authored messages or adopting a foreign graph.
			const auto revision = ChatRepository::ChatStorageRevision(app.data_root, root->id);
			const auto saved = ChatRepository::LoadLocalChat(app.data_root, root->id, true, nullptr, false);
			if (!revision || !saved || !InitialStoredChatMatches(app.data_root, *saved, *root) ||
			    ChatRepository::ChatStorageRevision(app.data_root, root->id) != revision)
			{
				return false;
			}
			// A pending authored save is not a failed delivery attempt.
			++run.root_result_delivery_attempts;

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
				tool.result_text = nlohmann::json{{"runId", run.id}, {"status", run.status}, {"agentId", run.agent_id}, {"providerId", run.provider_id}, {"executionCapability", run.execution_capability}, {"startedAtEpochMs", run.started_at_epoch_ms}, {"finishedAtEpochMs", run.finished_at_epoch_ms}, {"startedAt", run.started_at}, {"completedAt", run.finished_at}, {"transcriptChatId", run.transcript_chat_id}, {"result", run.result_excerpt}, {"diagnosticCode", run.diagnostic_code}, {"diagnostic", run.diagnostic}}.dump();
				tool.status = run.status == "completed" ? "completed" : "failed";
				tool.is_sub_agent = true;
				tool.sub_agent_id = run.id;
				tool.sub_agent_title = run.agent_id;
				root->messages.back().tool_calls.push_back(std::move(tool));
				const auto prepared = ChatRepository::PrepareChatSave(app.data_root, *root, *revision);
				if (!ChatRepository::PublishPreparedChatSave(prepared, true, root))
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
		               std::string diagnostic_code = {}, std::string diagnostic = {}, bool stop_already_confirmed = false)
		{
			if (ChatSession* chat = ChatDomainService().FindChatById(app, run.transcript_chat_id); chat != nullptr)
			{
				run.result_excerpt = ResultExcerpt(*chat);
			}
			if (!PersistStatus(app, run, std::move(status), std::move(diagnostic_code), std::move(diagnostic)))
			{
				app.status_line = "The final agent-run state could not be persisted; UAM will retry.";
				return;
			}
			(void)DeliverRootResult(app, run);
			app.agent_run_deadline_steady_ms.erase(run.id);
			if (stop_already_confirmed) EraseRuntimeChat(app, run.transcript_chat_id);
			else RemoveRuntimeChat(app, run.transcript_chat_id);
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

	namespace
	{
		std::string InitialConfiguration(const AppState& app, const ChatSession& chat)
		{
			const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			const auto* provider = ProviderResolutionService().ProviderForChat(app, chat);
			if (!host || !provider)
				return {};
			const auto& settings = app.settings;
			const auto& defaults = provider_chat_defaults::ForProvider(settings, provider->id, host->id);
			nlohmann::json application_rules = nlohmann::json::array();
			for (const auto& rule : settings.computer_use_allowed_applications)
				application_rules.push_back({rule.identity_kind, rule.identity});
			// Execution policy is independent of presentation, update observations and other UI preferences.
			const auto execution_settings = nlohmann::json::array({provider_availability::IsEnabled(settings, provider->id, host->id), settings.provider_extra_flags, provider_ids::NormalizeCliProviderAliasOrSelf(settings.permission_reviewer_provider_id), strings::Trim(settings.permission_reviewer_model_id), settings_frontend_json::SerializeMarkdownStoreDirectory(settings), central_configuration::Serialize(settings.central_provider_configuration), mcp_server_config::Serialize(host_scoped_settings::McpServers(settings, host->id)), nlohmann::json::array({defaults.model_id, defaults.approval_mode, defaults.command_safety_tier, defaults.reasoning_effort, defaults.service_tier, defaults.small_model_mode, defaults.memory_enabled, defaults.memory_level, defaults.feature_preference}), settings.git_enabled, settings.git_auto_worktree, settings.git_snapshot_svn, settings.git_snapshot_plain_folders, settings.git_pull_before_chat, settings.git_default_base_branch, settings.svn_enabled, settings.svn_update_before_chat, memory_policy::Resolve(settings, chat), settings.memory_recall_budget_bytes, settings.computer_use_globally_enabled, settings.computer_use_allowlist_enabled, application_rules, settings.acp_turn_output_limit_mib, settings.active_turn_inactivity_timeout_seconds, settings.acp_setup_inactivity_timeout_seconds});
			return nlohmann::json::array({execution_settings, host->id, host->transport, host->ssh_alias, host->platform, host->architecture, host->runner_version, host->runner_protocol_version, host->runner_directory, host->instruction_file, host->instruction_text ? nlohmann::json(*host->instruction_text) : nlohmann::json(nullptr), provider->id, provider->interactive_command, provider->runtime_flags, provider->resume_argument, provider->supports_structured, provider->structured_protocol, provider->execution_mode, provider->output_mode, provider->prompt_bootstrap, provider->prompt_bootstrap_path}).dump();
		}
		std::string InitialChildPolicy(const ChatSession& chat, const std::string* comparison_model = nullptr)
		{
			return nlohmann::json::array({chat.id, chat.agent_run_id, chat.managed_followup_incarnation, chat.created_at, chat.provider_id, chat.folder_id, chat.execution_host_id, chat.workspace_directory, chat.workspace_source_directory, chat.workspace_worktree_directory, chat.workspace_isolation_kind, chat.routing_mode, chat.routing_source_folder_id, chat.routing_source_directory, chat.routing_assigned_connection_identity, chat.routing_assigned_provider_configuration_identity, chat.routing_inherited_default_fields, comparison_model ? *comparison_model : chat.model_id, chat.reasoning_effort, chat.service_tier, chat.service_tier_explicit, chat.approval_mode, chat.command_safety_tier, chat.uam_agent_id, chat.uam_control_enabled, chat.uam_control_explicitly_disabled, chat.active_goal_id, chat.goal_owner_chat_id, chat.temporary_parent_chat_id, chat.imported_read_only, chat.side_cleanup_requested, chat.extra_flags}).dump();
		}
		bool InitialStoredChatMatches(const std::filesystem::path& root, ChatSession saved, const ChatSession& captured)
		{
			if (!saved.messages_loaded || saved.messages != captured.messages)
				return false;
			// These two fields are load-cache projections; exact loaded bodies already establish the prefix.
			// The detailed publisher advances the full-save receipt; its own queue and full-primary CAS remain mandatory.
			// Every authored metadata field, queue storage owner/revision and birth remains exact.
			saved.persisted_message_count = captured.persisted_message_count;
			saved.persisted_messages_digest = captured.persisted_messages_digest;
			saved.acp_queue_full_save_revision = captured.acp_queue_full_save_revision;
			return ChatRepository::ChatMetadataFingerprint(root, saved) == ChatRepository::ChatMetadataFingerprint(root, captured);
		}

		bool InitialRunEquals(const AgentRun& a, const AgentRun& b)
		{
			return std::tie(a.managed_continuation, a.id, a.root_chat_id, a.parent_run_id, a.resumed_from_run_id, a.transcript_chat_id, a.goal_id, a.agent_id, a.definition_hash, a.definition_snapshot, a.definition_instructions, a.skills_snapshot, a.delegates_snapshot, a.provider_id, a.model_id, a.execution_capability, a.task, a.effective_workspace_access, a.status, a.depth, a.expected_turn_serial, a.started_at_epoch_ms, a.finished_at_epoch_ms, a.deadline_at_epoch_ms, a.created_at, a.started_at, a.finished_at, a.updated_at, a.result_delivery_id, a.deliver_result_to_root_chat, a.root_result_delivered, a.root_result_delivery_attempts, a.result_excerpt, a.diagnostic_code, a.diagnostic) == std::tie(b.managed_continuation, b.id, b.root_chat_id, b.parent_run_id, b.resumed_from_run_id, b.transcript_chat_id, b.goal_id, b.agent_id, b.definition_hash, b.definition_snapshot, b.definition_instructions, b.skills_snapshot, b.delegates_snapshot, b.provider_id, b.model_id, b.execution_capability, b.task, b.effective_workspace_access, b.status, b.depth, b.expected_turn_serial, b.started_at_epoch_ms, b.finished_at_epoch_ms, b.deadline_at_epoch_ms, b.created_at, b.started_at, b.finished_at, b.updated_at, b.result_delivery_id, b.deliver_result_to_root_chat, b.root_result_delivered, b.root_result_delivery_attempts, b.result_excerpt, b.diagnostic_code, b.diagnostic);
		}
		bool InitialRootIdle(const AppState& app, const ChatSession& chat, bool require_loaded = true, const BranchPreparationOwnership* allowed = nullptr, bool allow_deferred_save = false)
		{
			const bool save_worker = allow_deferred_save ? acp_detail::HasActiveChatSaveWorker(app, chat.id) : app.pending_chat_save_tasks.contains(chat.id);
			if (ChatHasBusyCliTerminal(app, chat.id) || app.deletion_progress || (require_loaded && !chat.messages_loaded) || !chat.metadata_incarnation || chat.imported_read_only || chat.side_cleanup_requested || !chat.agent_run_id.empty() || !chat.temporary_parent_chat_id.empty() || !chat.goal_owner_chat_id.empty() || chat.managed_followup_input_unavailable || chat.acp_queue_storage_error || !chat.acp_queued_prompts.empty() || !chat.remote_pending_requests.empty() || chat.remote_turn_reconnect_pending || chat.remote_stop_cleanup_pending || chat.remote_restart_pending || save_worker || (!allow_deferred_save && app.pending_chat_save_at_by_chat_id.contains(chat.id)) || app.worktree_operation_chat_ids.contains(chat.id) || app.worktree_launch_preparations.contains(chat.id) || app.execution_routing_restored_unresolved_chat_ids.contains(chat.id))
				return false;
			const auto branch = app.branch_preparations.find(chat.id);
			if (branch != app.branch_preparations.end() && !branch->second.expired() && branch->second.lock().get() != allowed)
				return false;
			const auto* session = FindAcpSessionForChat(app, chat.id);
			return !session || (!AcpSessionHasActiveTurn(*session) && !session->processing && !session->reconnect_pending && !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->local_stop_pending && !session->pending_agent_input && !session->pending_local_launch && !session->pending_local_session_receipt && !session->pending_prompt_delivery && session->queued_user_prompts.empty() && session->queued_prompt.empty());
		}
		bool InitialCreationOwned(const AppState& app, const PendingManagedInitialMention& task)
		{
			const auto* current = ChatDomainService().FindChatById(app, task.root_id);
			const auto& work = task.work;
			return !task.cancelled && work && !work->cancel.load() && InitialClock::now() < task.deadline && app.data_root == task.root && current && InitialRootIdle(app, *current) && std::ranges::count(app.chats, task.root_id, &ChatSession::id) == 1 && std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status) < kMaxQueuedAgents && std::ranges::count_if(app.agent_runs, [&](const AgentRun& run) { return run.root_chat_id == task.root_id && run.parent_run_id.empty() && !IsTerminal(run.status); }) < kMaxDirectChildren && current->metadata_incarnation == task.incarnation && current->messages == work->before.messages && current->created_at == work->before.created_at && ChatRepository::ChatMetadataFingerprint(app.data_root, *current) == task.metadata && InitialConfiguration(app, *current) == task.settings && std::ranges::count(app.chats, task.transcript_id, &ChatSession::id) == 0 && std::ranges::count(app.agent_runs, work->run.id, &AgentRun::id) == 0;
		}
		bool InitialChildPolicyMatches(const AppState& app, const ManagedInitialRuntimeOwner& owner, const ChatSession& chat)
		{
			if (InitialChildPolicy(chat) == owner.child_policy)
				return true;
			// Compare only an alias resolved by this exact owned session's native catalogue.
			// Model acknowledgement and prompt authority remain separate runtime gates.
			const auto* session = FindAcpSessionForChat(app, chat.id);
			if (provider_ids::NormalizeCliProviderAliasOrSelf(owner.expected.provider_id) != provider_ids::kOpenCodeCli || session != owner.session || !session || session->managed_initial_owner.get() != &owner || session->provider_id != provider_ids::kOpenCodeCli || !session->running || !session->initialized || !session->session_ready || owner.process.empty() || session->last_process_id != owner.process || owner.expected.model_id.empty())
				return false;
			std::string error;
			const auto resolved = acp_models::ResolveAdvertisedModelId(*session, owner.expected.model_id, error);
			return resolved && *resolved == chat.model_id && InitialChildPolicy(chat, &owner.expected.model_id) == owner.child_policy;
		}
		bool FollowupProtocolMatches(std::string_view provider, std::string_view protocol)
		{
			return (provider == "claude-cli" && protocol == "claude-code-stream-json") || (provider == "codex-cli" && protocol == "codex-app-server") || (provider == "copilot-cli" && protocol == "copilot-sdk");
		}
		bool FollowupShape(const AgentRun& run)
		{
			if (!run.managed_continuation || run.depth != 1 || !run.parent_run_id.empty() || !run.resumed_from_run_id.empty() || !run.goal_id.empty() || (run.provider_id != "claude-cli" && run.provider_id != "codex-cli" && run.provider_id != "copilot-cli")) return false;
			const auto& value = *run.managed_continuation;
			if (value.owner.delegation_grant || !value.execution || value.turns.empty() || value.turns.size() > 33 || value.current_turn_id != value.turns.back().turn_id || value.current_turn_number != value.turns.size() || !uuid::IsCanonicalUuid(value.owner.root_birth_id) || !uuid::IsCanonicalUuid(value.owner.transcript_birth_id)) return false;
			const auto& binding = *value.execution;
			if (binding.execution_host_id != "local" || binding.routing_mode != "explicit" || !uuid::IsCanonicalUuid(binding.native_session_id) || binding.effective_model_id.empty() || strings::ToLowerAscii(binding.effective_model_id) == "auto" || strings::ToLowerAscii(binding.effective_model_id) == "default" || binding.effective_model_id != run.model_id || binding.model_identity_source != "exact-requested" || binding.connection_configuration_json.empty() || binding.provider_configuration_json.empty()) return false;
			if ((run.provider_id == "codex-cli" || run.provider_id == "copilot-cli") && (run.effective_workspace_access != "read" || binding.approval_mode != approval_modes::kPlanApprovalMode)) return false;
			return std::ranges::all_of(value.turns, [&](const auto& turn) { return turn.principal.kind == ManagedRunPrincipalKind::UserRoot && turn.principal.root_chat_id == run.root_chat_id && turn.principal.root_birth_id == value.owner.root_birth_id && turn.principal.parent_run_id.empty() && turn.principal.parent_transcript_birth_id.empty(); });
		}
		bool ReadPolicyResumeEvidenceMatches(const ManagedInitialRuntimeOwner& owner)
		{
			if (!owner.resume_only || (owner.expected.provider_id != "codex-cli" && owner.expected.provider_id != "copilot-cli")) return true;
			const auto& value = owner.expected.managed_continuation;
			if (!value || !value->execution) return false;
			const auto& binding = *value->execution;
			return !owner.binding_withheld && owner.setup_execution_observed && owner.native_response_seen && owner.native_observed == binding.native_session_id && owner.observed_native == binding.native_session_id && owner.observed_model == binding.effective_model_id && owner.observed_workspace == binding.workspace_directory && owner.observed_mode == approval_modes::kPlanApprovalMode;
		}
		bool FollowupConfigurationMatches(const AppState& app, const ChatSession& chat, const AgentRun& run)
		{
			if (!FollowupShape(run)) return false;
			const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
			const auto* provider = ProviderResolutionService().ProviderForChat(app, chat);
			if (!host || !provider || host->id != "local" || host->transport != "local" || provider->id != run.provider_id || !provider->supports_resume || !provider->supports_structured || !FollowupProtocolMatches(provider->id, provider->structured_protocol) || !ProviderResolutionService().ChatProviderIsAvailable(app, chat) || !paths::IsControllerLocalWorkspace(chat)) return false;
			auto current = *run.managed_continuation->execution;
			current.connection_configuration_json = ExecutionRoutingConnectionConfiguration(*host);
			current.provider_configuration_json = ExecutionRoutingProviderConfiguration(app, *provider, host->id);
			current.execution_host_id = chat.execution_host_id;
			current.workspace_directory = chat.workspace_directory;
			current.workspace_worktree_directory = chat.workspace_worktree_directory;
			current.workspace_source_directory = chat.workspace_source_directory;
			current.routing_mode = chat.routing_mode;
			current.routing_source_folder_id = chat.routing_source_folder_id;
			current.routing_source_directory = chat.routing_source_directory;
			current.effective_model_id = chat.model_id;
			current.reasoning_effort = chat.reasoning_effort;
			current.service_tier = chat.service_tier;
			current.service_tier_explicit = chat.service_tier_explicit;
			current.approval_mode = chat.approval_mode;
			current.command_safety_tier = chat.command_safety_tier;
			current.native_session_id = chat.native_session_id;
			return current == *run.managed_continuation->execution && chat.agent_run_id == run.id && chat.managed_followup_incarnation == run.managed_continuation->owner.transcript_birth_id && !chat.small_model_mode && chat.active_goal_id.empty() && chat.goal_owner_chat_id.empty() && chat.temporary_parent_chat_id.empty();
		}
		bool InitialRuntimeOwned(const AppState& app, const ManagedInitialRuntimeOwner& owner, bool policy = true)
		{
			const auto* root = ChatDomainService().FindChatById(app, owner.root_id);
			const auto* transcript = ChatDomainService().FindChatById(app, owner.transcript_id);
			const auto* run = FindRun(app, owner.expected.id);
			if (app.data_root != owner.root || !root || !transcript || !run || !InitialRunEquals(*run, owner.expected) || root->metadata_incarnation != owner.root_incarnation || transcript->metadata_incarnation != owner.transcript_incarnation || root->created_at != owner.root_created || transcript->created_at != owner.transcript_created || root->managed_followup_incarnation != owner.root_birth || transcript->managed_followup_incarnation != owner.transcript_birth || root->imported_read_only || root->side_cleanup_requested || !root->agent_run_id.empty() || transcript->agent_run_id != run->id || transcript->side_cleanup_requested || transcript->imported_read_only || std::ranges::count(app.agent_runs, owner.expected.id, &AgentRun::id) != 1 || std::ranges::count(app.agent_runs, owner.transcript_id, &AgentRun::transcript_chat_id) != 1 || std::ranges::count(app.chats, owner.root_id, &ChatSession::id) != 1 || std::ranges::count(app.chats, owner.transcript_id, &ChatSession::id) != 1)
				return false;
			if (policy && ((owner.resume_only && !FollowupConfigurationMatches(app, *transcript, owner.expected)) || !InitialChildPolicyMatches(app, owner, *transcript) || app.worktree_operation_chat_ids.contains(owner.transcript_id) || app.worktree_launch_preparations.contains(owner.transcript_id) || app.execution_routing_restored_unresolved_chat_ids.contains(owner.transcript_id) || (!owner.input_written && (InitialConfiguration(app, *transcript) != owner.settings || InitialChildPolicy(*root) != owner.root_policy || InitialConfiguration(app, *root) != owner.root_settings || app.worktree_operation_chat_ids.contains(owner.root_id) || app.worktree_launch_preparations.contains(owner.root_id) || app.execution_routing_restored_unresolved_chat_ids.contains(owner.root_id)))))
				return false;
			return true;
		}
		bool InitialNeedsOwner(const AppState& app, const ChatSession& chat)
		{
			if (chat.agent_run_id.empty())
				return false;
			const auto* run = FindRun(app, chat.agent_run_id);
			return run ? run->managed_continuation.has_value() : !chat.managed_followup_incarnation.empty();
		}
		std::shared_ptr<ManagedInitialRuntimeOwner> InitialOwner(const AppState& app, const ChatSession& chat)
		{
			const auto found = app.managed_initial_runtime_owners.find(chat.agent_run_id);
			return found == app.managed_initial_runtime_owners.end() ? nullptr : found->second;
		}
		std::shared_ptr<ManagedInitialRuntimeOwner> ImmutableManagedReadOwner(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const ManagedNativeReadScope& scope)
		{
			const auto owner = session.managed_initial_owner;
			// Revocation follows the immutable transaction owner, even after policy drift or settlement.
			// Fresh policy/configuration equality is deliberately required only when granting proof.
			if (!owner || owner->session != &session || InitialOwner(app, chat) != owner || FindAcpSessionForChat(app, chat.id) != &session || ChatDomainService().FindChatById(app, chat.id) != &chat || app.data_root != owner->root || owner->expected.id != scope.run_id || owner->expected.transcript_chat_id != chat.id || chat.agent_run_id != scope.run_id || session.managed_agent_run_id != scope.run_id || owner->process.empty() || owner->process != scope.process_id || session.last_process_id != scope.process_id || owner->serial != scope.turn_serial || session.turn_serial != scope.turn_serial || owner->root_birth != scope.root_birth_id || owner->transcript_birth != scope.transcript_birth_id || owner->resume_only != scope.resume_only || owner->expected.provider_id != "copilot-cli" || session.provider_id != "copilot-cli" || session.protocol_kind != "copilot-sdk") return nullptr;
			const auto& managed = owner->expected.managed_continuation;
			const auto* root = ChatDomainService().FindChatById(app, owner->root_id);
			const auto* run = FindRun(app, scope.run_id);
			if (!managed || managed->current_turn_id != scope.turn_id || managed->owner.root_birth_id != scope.root_birth_id || managed->owner.transcript_birth_id != scope.transcript_birth_id || !root || root->metadata_incarnation != owner->root_incarnation || root->created_at != owner->root_created || root->managed_followup_incarnation != scope.root_birth_id || chat.metadata_incarnation != owner->transcript_incarnation || chat.created_at != owner->transcript_created || chat.managed_followup_incarnation != scope.transcript_birth_id || !run || run->root_chat_id != owner->root_id || run->transcript_chat_id != chat.id || !run->managed_continuation || run->managed_continuation->current_turn_id != scope.turn_id || run->managed_continuation->owner != managed->owner || (scope.resume_only ? (!managed->execution || managed->execution->native_session_id != scope.native_session_id) : !scope.native_session_id.empty())) return nullptr;
			return owner;
		}
		void ForbidInitialInput(ManagedInitialRuntimeOwner& owner, std::string diagnostic = "Managed initial ownership changed before input.")
		{
			owner.input_forbidden = true;
			if (owner.desired_status.empty())
			{
				owner.desired_status = "failed";
				owner.diagnostic_code = "initial_ownership_changed";
				owner.diagnostic = std::move(diagnostic);
			}
		}
		bool InitialStopped(const AppState& app, const ManagedInitialRuntimeOwner& owner)
		{
			if (owner.orphan_stop_transferred && owner.stop_finished && owner.stop_finished->load(std::memory_order_acquire))
				return owner.stop_result && owner.stop_result->exit_confirmed;
			const auto* session = FindAcpSessionForChat(app, owner.transcript_id);
			if (session && (session->running || session->local_stop_pending || session->remote_stop_pending || session->remote_stop_unconfirmed || session->pending_local_launch || session->worktree_launch_pending || session->pending_agent_input))
				return false;
			return std::ranges::none_of(app.acp_process_stop_tasks, [&](const auto& task) { return task.chat_id == owner.transcript_id; }) && std::ranges::none_of(app.pending_acp_remote_stops, [&](const auto& task) { return task && task->chat_id == owner.transcript_id; });
		}
		bool InitialReadOnlyStoppedLeaseOwned(const AppState& app, const ManagedInitialRuntimeOwner& owner, const AcpSessionState& session, const ChatSession& chat)
		{
			const auto* execution = owner.expected.managed_continuation && owner.expected.managed_continuation->execution ? &*owner.expected.managed_continuation->execution : nullptr;
			// MarkAcpProcessExited alone records known exit codes; Reset clears them with last_process_id.
			// Missing handles/unknown (-1) codes and remote proxy exits cannot establish this local observation.
			const bool observed_local_exit = owner.resume_only && session.has_last_exit_code && session.last_exit_code >= 0 && execution && session.provider_id == "claude-cli" && owner.expected.provider_id == session.provider_id && session.protocol_kind == "claude-code-stream-json" && session.process_execution_host_id == "local" && chat.execution_host_id == "local" && execution->execution_host_id == "local" && session.managed_launch_attempted && session.managed_agent_run_id == owner.expected.id && session.turn_serial == owner.serial && chat.native_session_id == execution->native_session_id && session.session_id == execution->native_session_id;
			const bool tagged_stop = owner.stop_finished && owner.stop_finished->load(std::memory_order_acquire) && owner.stop_result && owner.stop_result->exit_confirmed;
			return owner.input_forbidden && owner.completion_acked && owner.session == &session && session.managed_initial_owner.get() == &owner && session.chat_id == owner.transcript_id && chat.id == owner.transcript_id && ChatDomainService().FindChatById(app, owner.transcript_id) == &chat && !session.running && !session.processing && session.lifecycle_state == acp_detail::kAcpLifecycleStopped && !session.reconnect_pending && !session.recovering_remote_turn && !session.recovering_remote_process && !session.pending_prompt_delivery && !session.pending_local_session_receipt && !session.pending_queued_history && session.queued_prompt.empty() && session.queued_user_prompts.empty() && !owner.process.empty() && session.last_process_id == owner.process && (tagged_stop || observed_local_exit) && InitialStopped(app, owner) && InitialRuntimeOwned(app, owner, false);
		}
		template <class T, class F> bool StartInitialPreparation(std::shared_ptr<T> work, F function)
		{
			work->finished.store(false, std::memory_order_release);
			try
			{
				std::thread(
				    [work, function = std::move(function)]() mutable
				    {
					    try
					    {
						    function(*work);
					    }
					    catch (...)
					    {
						    work->error = "Managed preparation failed. The task was not replayed.";
					    }
					    work->finished.store(true, std::memory_order_release);
				    })
				    .detach();
				return true;
			}
			catch (...)
			{
				work->error = "Managed preparation could not start.";
				work->finished.store(true, std::memory_order_release);
				return false;
			}
		}
		void CleanInitialCreation(PendingManagedInitialMention& task)
		{
			task.phase = PendingManagedInitialMention::Phase::Cleaning;
			if (task.work)
				task.work->cancel.store(true);
			task.work.reset();
		}
	} // namespace

	bool AgentRunScheduler::HasPendingInitialMention(const AppState& app, std::string_view chat_id)
	{
		const auto* current = ChatDomainService().FindChatById(app, std::string(chat_id));
		if (current && HasUnconfirmedInitialRootPublication(app, *current))
			return true;
		if (std::ranges::any_of(app.managed_initial_mentions,
		                        [&](const auto& entry)
		                        {
			                        const auto& task = entry.second;
			                        if (!task || task->root != app.data_root || (task->root_id != chat_id && task->transcript_id != chat_id))
				                        return false;
			                        const auto* root = ChatDomainService().FindChatById(app, task->root_id);
			                        if (!root || root->metadata_incarnation != task->incarnation || root->created_at != task->root_created || root->managed_followup_incarnation != task->root_birth)
				                        return false;
			                        if (task->transcript_id == chat_id)
			                        {
				                        const auto* child = ChatDomainService().FindChatById(app, task->transcript_id);
				                        if (child && !task->accepted)
					                        return false;
				                        if (child && (child->agent_run_id != task->accepted_run_id || child->metadata_incarnation != task->accepted_transcript_incarnation || child->created_at != task->accepted_transcript_created || child->managed_followup_incarnation != task->accepted_transcript_birth))
					                        return false;
			                        }
			                        return true; // Includes the real unresolved publication ticket after shutdown worker retirement.
		                        }))
			return true;
		if (std::ranges::any_of(app.managed_followups, [&](const auto& entry)
		{
			const auto& task = entry.second;
			return task && !task->replay && (task->root_id == chat_id || task->transcript_id == chat_id) && FollowupTaskScope(app, *task);
		})) return true;
		return std::ranges::any_of(app.managed_initial_runtime_owners,
		                           [&](const auto& entry)
		                           {
			                           const auto& owner = entry.second;
			                           return owner && (owner->root_id == chat_id || owner->transcript_id == chat_id) && InitialRuntimeOwned(app, *owner, false) && (owner->transition || (owner->retired && !owner->retired->load(std::memory_order_acquire)));
		                           });
	}

	bool AgentRunScheduler::HasUnconfirmedInitialRootPublication(const AppState& app, const ChatSession& chat)
	{
		const auto found = app.managed_initial_mentions.find(chat.id);
		if (found != app.managed_initial_mentions.end() && found->second)
		{
			const auto& task = *found->second;
			if (task.uncertain_root && task.root == app.data_root && task.root_id == chat.id && task.incarnation == chat.metadata_incarnation && task.root_created == chat.created_at && task.root_birth == chat.managed_followup_incarnation && (task.phase == PendingManagedInitialMention::Phase::ReconcilingRoot || task.phase == PendingManagedInitialMention::Phase::Cleaning || task.phase == PendingManagedInitialMention::Phase::UncertainShutdown))
				return true;
		}
		// A result publication owns the ROOT evidence independently of the child's current namespace.
		return std::ranges::any_of(app.managed_initial_runtime_owners,
		                           [&](const auto& entry)
		                           {
			                           const auto& owner = entry.second;
			                           return owner && owner->parent_result_uncertain && owner->root == app.data_root && owner->root_id == chat.id && owner->root_incarnation == chat.metadata_incarnation && owner->root_created == chat.created_at && owner->root_birth == chat.managed_followup_incarnation;
		                           });
	}

	void AgentRunScheduler::CancelPendingInitialMention(AppState& app, std::string_view chat_id)
	{
		for (const auto& [id, task] : app.managed_followups)
		{
			if (!task || task->replay || (task->root_id != chat_id && task->transcript_id != chat_id) || !FollowupTaskScope(app, *task)) continue;
			task->cancelled = true;
			if (task->work) task->work->cancel.store(true, std::memory_order_release);
			if (const auto owner = app.managed_initial_runtime_owners.find(id); owner != app.managed_initial_runtime_owners.end() && InitialRuntimeOwned(app, *owner->second, false))
			{
				owner->second->input_forbidden = true; owner->second->desired_status = "cancelled";
				owner->second->diagnostic_code = "user_cancelled"; owner->second->diagnostic = "The user cancelled this managed follow-up.";
				app.pending_agent_run_cancellation_ids.insert(id);
			}
		}
		for (const auto& [id, task] : app.managed_initial_mentions)
		{
			if (!task || (task->root_id != chat_id && task->transcript_id != chat_id) || task->root != app.data_root)
				continue;
			const auto* root = ChatDomainService().FindChatById(app, task->root_id);
			if (!root || root->metadata_incarnation != task->incarnation || root->created_at != task->root_created)
				continue;
			task->cancelled = true;
			if (task->work)
				task->work->cancel.store(true, std::memory_order_release);
			const auto runtime = app.managed_initial_runtime_owners.find(task->accepted_run_id);
			if (!app.managed_initial_shutdown && runtime != app.managed_initial_runtime_owners.end() && InitialRuntimeOwned(app, *runtime->second, false))
			{
				runtime->second->input_forbidden = true;
				runtime->second->desired_status = "cancelled";
				runtime->second->diagnostic_code = "user_cancelled";
				runtime->second->diagnostic = "The user cancelled this agent run.";
				app.pending_agent_run_cancellation_ids.insert(runtime->first);
			}
		}
	}

	bool AgentRunScheduler::OwnsInitialRoutingPreparation(const AppState& app, const ChatSession& chat)
	{
		if (app.execution_routing_restored_unresolved_chat_ids.contains(chat.id))
			return false;
		const auto pending = app.managed_initial_mentions.find(chat.id);
		if (pending != app.managed_initial_mentions.end() && pending->second && pending->second->work && pending->second->phase == PendingManagedInitialMention::Phase::PreparingRoot && InitialCreationOwned(app, *pending->second))
			return true;
		for (const auto& [id, result_owner] : app.managed_initial_runtime_owners)
			if (result_owner && result_owner->root_id == chat.id && result_owner->transition && result_owner->action == ManagedInitialRuntimeOwner::Action::Parent && InitialRuntimeOwned(app, *result_owner, false) && ChatRepository::ChatMetadataFingerprint(app.data_root, chat) == result_owner->pending_parent_metadata)
				return true;
		const auto owner = InitialOwner(app, chat);
		return owner && owner->phase == ManagedInitialRuntimeOwner::Phase::Queued && owner->transition && owner->action == ManagedInitialRuntimeOwner::Action::Running && !owner->input_forbidden && owner->completion_acked && app.execution_routing_reserved_chat_ids.contains(chat.id) && !chat.routing_assigned_connection_identity.empty() && InitialRuntimeOwned(app, *owner);
	}

	ManagedInitialMentionAdmission AgentRunScheduler::TryBeginInitialMention(AppState& app, const std::string& root_id, const std::string& text, std::function<bool(const ManagedInitialMentionResult&)> completion, std::string* error_out, const std::function<bool(ChatPublicationFile, io::AtomicWriteStage)>& root_stage_hook_for_tests, const std::function<bool(ChatPublicationFile, io::AtomicWriteStage)>& parent_stage_hook_for_tests)
	{
		if (text.size() < 3 || text.front() != '@' || text[1] == '@')
			return ManagedInitialMentionAdmission::NotApplicable;
		std::size_t end = 1;
		while (end < text.size() && (std::islower(static_cast<unsigned char>(text[end])) || std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-'))
			++end;
		if (end == 1 || end > 65 || end == text.size() || !std::isspace(static_cast<unsigned char>(text[end])))
			return ManagedInitialMentionAdmission::NotApplicable;
		const auto* root = ChatDomainService().FindChatById(app, root_id);
		if (!root || !root->agent_run_id.empty() || !root->temporary_parent_chat_id.empty() || !root->goal_owner_chat_id.empty())
			return ManagedInitialMentionAdmission::NotApplicable;
		// Preserve the legacy recognition-only lookup: unknown names allocate no task or receipt.
		const auto workspace = paths::ResolveControllerWorkspaceRootPath(app, *root);
		const auto catalog = AgentDefinitionService::Load(app.data_root, workspace);
		const auto* definition = FindAgent(catalog, text.substr(1, end - 1));
		if (!definition || (definition->mode != "subagent" && definition->mode != "both"))
			return ManagedInitialMentionAdmission::NotApplicable;
		const auto reject = [&](const char* reason)
		{
			if (error_out)
				*error_out = reason;
			return ManagedInitialMentionAdmission::Rejected;
		};
		if (!completion || app.managed_initial_shutdown || text.size() > 1024 * 1024 || strings::Trim(text.substr(end)).empty())
			return reject("Add a bounded task after the agent name.");
		if (HasPendingInitialMention(app, root_id) || !InitialRootIdle(app, *root))
			return reject("Wait for pending chat work before delegating this request.");
		if (std::ranges::count_if(app.agent_runs, [&](const AgentRun& run) { return run.root_chat_id == root_id && run.parent_run_id.empty() && !IsTerminal(run.status); }) >= kMaxDirectChildren)
			return reject("This parent already has four active managed children.");
		if (std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status) + app.managed_initial_mentions.size() >= kMaxQueuedAgents)
			return reject("The bounded managed-agent queue is full.");
		const auto provider = provider_ids::NormalizeCliProviderAliasOrSelf(root->provider_id);
		if (!ProviderExists(app, provider) || !ProviderResolutionService().ChatProviderIsAvailable(app, *root))
			return reject("The selected provider is unavailable.");
		const std::string configuration = InitialConfiguration(app, *root);
		if (configuration.empty())
			return reject("The selected execution host is unavailable.");
		auto work = native_history::TryCreateWorkerOwnedNativeRefresh<InitialPreparation>();
		if (!work)
			return reject("Bounded managed preparation is busy. Retry after it finishes.");
		auto task = std::make_shared<PendingManagedInitialMention>();
		task->root = app.data_root;
		task->root_id = root_id;
		task->incarnation = root->metadata_incarnation;
		task->root_created = root->created_at;
		task->root_birth = root->managed_followup_incarnation;
		task->metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *root);
		task->settings = configuration;
		task->completion = std::move(completion);
		task->root_stage_hook_for_tests = root_stage_hook_for_tests;
		task->work = work;
		work->retired = task->retired;
		task->parent_stage_hook_for_tests = parent_stage_hook_for_tests;
		work->root = app.data_root;
		work->before = *root;
		work->parent = *root;
		if (work->parent.managed_followup_incarnation.empty())
			work->parent.managed_followup_incarnation = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (!uuid::IsCanonicalUuid(work->parent.managed_followup_incarnation))
			return reject("The parent chat could not acquire its durable identity.");
		ChatDomainService().AddMessage(work->parent, MessageRole::User, text);
		work->transcript = ChatDomainService().CreateNewChat(root->folder_id, provider);
		auto& transcript = work->transcript;
		transcript.managed_followup_incarnation = PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (!uuid::IsCanonicalUuid(transcript.managed_followup_incarnation) || transcript.managed_followup_incarnation == work->parent.managed_followup_incarnation)
			return reject("The managed transcript could not acquire its durable identity.");
		transcript.title = "Agent: " + definition->id;
		transcript.execution_host_id = root->execution_host_id;
		transcript.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, *root));
		transcript.workspace_source_directory = root->workspace_source_directory;
		transcript.routing_mode = root->routing_mode;
		transcript.routing_source_folder_id = root->routing_source_folder_id;
		transcript.routing_source_directory = root->routing_source_directory;
		if (transcript.routing_mode == "automatic")
		{
			if (const auto* source = ChatDomainService().FindFolderById(app, transcript.routing_source_folder_id))
			{
				transcript.folder_id = source->id;
				transcript.execution_host_id = source->execution_host_id;
				transcript.workspace_directory = transcript.routing_source_directory;
			}
		}
		transcript.model_id = root->model_id;
		transcript.reasoning_effort = root->reasoning_effort;
		transcript.service_tier = root->service_tier;
		transcript.service_tier_explicit = root->service_tier_explicit;
		const auto* root_session = FindAcpSessionForChat(app, root_id);
		const auto* root_agent = FindAgent(catalog, root->uam_agent_id);
		const bool read_only = definition->workspace_access == "read" || (root_session ? root_session->active_uam_agent_workspace_access == "read" : root_agent && root_agent->workspace_access == "read") || approval_modes::AppApprovalModeOrEmpty(root->approval_mode) == approval_modes::kPlanApprovalMode;
		transcript.approval_mode = read_only ? approval_modes::kPlanApprovalMode : root->approval_mode;
		transcript.command_safety_tier = root->command_safety_tier;
		transcript.uam_agent_id = definition->id;
		transcript.uam_control_enabled = root->uam_control_enabled;
		transcript.uam_control_explicitly_disabled = root->uam_control_explicitly_disabled;
		auto& run = work->run;
		run.goal_id = root->active_goal_id;
		run.id = AgentRunLedger::NewRunId();
		// Presentation correlation on the original user ACK, before ordered publication.
		work->parent.messages.back().managed_origin_run_id = run.id;
		run.root_chat_id = root_id;
		run.transcript_chat_id = transcript.id;
		run.agent_id = definition->id;
		run.definition_hash = definition->definition_hash;
		run.definition_snapshot = definition->markdown_snapshot;
		run.definition_instructions = definition->instructions;
		run.skills_snapshot = definition->skills;
		run.delegates_snapshot = definition->delegates;
		run.provider_id = provider;
		run.model_id = transcript.model_id;
		run.execution_capability = AgentDefinitionService::ExecutionCapabilityForProvider(provider);
		run.task = strings::Trim(text.substr(end));
		run.effective_workspace_access = read_only ? "read" : "write";
		run.depth = 1;
		run.status = "queued";
		run.created_at = run.updated_at = time::TimestampNow();
		run.result_delivery_id = run.id;
		run.deliver_result_to_root_chat = true;
		ManagedRunContinuation continuation;
		continuation.owner = {work->parent.managed_followup_incarnation, transcript.managed_followup_incarnation, {}};
		continuation.current_turn_id = run.id;
		continuation.current_turn_number = 1;
		continuation.state_revision = 1;
		ManagedRunTurnReceipt turn;
		turn.turn_id = turn.delivery_id = run.id;
		turn.turn_number = 1;
		turn.result_required = true;
		turn.principal = {ManagedRunPrincipalKind::UserRoot, root_id, work->parent.managed_followup_incarnation, {}, {}};
		continuation.turns.push_back(turn);
		run.managed_continuation = std::move(continuation);
		transcript.agent_run_id = run.id;
		task->transcript_id = transcript.id;
		app.managed_initial_mentions.emplace(root_id, task);
		const AgentDefinition selected = *definition;
		StartInitialPreparation(work,
		                        [workspace, selected](InitialPreparation& work)
		                        {
			                        const auto catalog = AgentDefinitionService::Load(work.root, workspace);
			                        const auto* current = FindAgent(catalog, selected.id);
			                        if (work.cancel.load() || !current || current->markdown_snapshot != selected.markdown_snapshot || current->definition_hash != selected.definition_hash)
			                        {
				                        work.error = "The delegated definition changed during preparation.";
				                        return;
			                        }
			                        const auto revision = ChatRepository::ChatStorageRevision(work.root, work.parent.id);
			                        if (!revision)
			                        {
				                        work.error = "Parent storage could not be inspected.";
				                        return;
			                        }
			                        work.root_was_absent = *revision == "missing";
			                        if (*revision != "missing")
			                        {
				                        const auto saved = ChatRepository::LoadLocalChat(work.root, work.before.id, true, nullptr, false);
				                        if (!saved || !InitialStoredChatMatches(work.root, *saved, work.before) || ChatRepository::ChatStorageRevision(work.root, work.before.id) != revision)
				                        {
					                        work.error = "The saved parent changed before managed preparation.";
					                        return;
				                        }
			                        }
			                        work.root_prepared = ChatRepository::PrepareChatPublication(work.root, work.parent, *revision == "missing" ? ChatPublicationMode::CreateAbsent : ChatPublicationMode::UpdateExisting, *revision == "missing" ? std::string{} : *revision, &work.error);
			                        if (!work.root_prepared || work.cancel.load())
				                        return;
			                        work.transcript_prepared = ChatRepository::PrepareChatPublication(work.root, work.transcript, ChatPublicationMode::CreateAbsent, {}, &work.error);
			                        if (!work.transcript_prepared || work.cancel.load())
				                        return;
			                        work.ledger_prepared = AgentRunLedger::PrepareInitialSave(work.root, work.run, &work.error);
		                        });
		return ManagedInitialMentionAdmission::Pending;
	}

	namespace
	{
		bool PollInitialCreations(AppState& app)
		{
			bool changed = false;
			std::vector<std::string> ids;
			for (const auto& [id, task] : app.managed_initial_mentions)
				ids.push_back(id);
			for (const auto& id : ids)
			{
				const auto entry = app.managed_initial_mentions.find(id);
				if (entry == app.managed_initial_mentions.end())
					continue;
				auto task = entry->second;
				if (task->phase == PendingManagedInitialMention::Phase::PreparingRoot)
				{
					if (app.managed_initial_shutdown || !InitialCreationOwned(app, *task))
					{
						task->cancelled = true;
						task->work->cancel.store(true);
					}
					if (!task->work->finished.load(std::memory_order_acquire))
						continue;
					auto work = task->work;
					const auto owned = [&] { return InitialCreationOwned(app, *task); };
					if (!owned() || !work->root_prepared || !work->transcript_prepared || !work->ledger_prepared || !work->error.empty())
					{
						task->result.error = work->error.empty() ? "Managed request ownership changed before it was recorded." : work->error;
						CleanInitialCreation(*task);
						continue;
					}
					// No owner yield or pump between detailed ROOT publication and exact live ACK.
					ChatSession committed_parent = work->parent;
					const auto parent_result = ChatRepository::PublishPreparedChatPublication(work->root_prepared, owned, &committed_parent, task->root_stage_hook_for_tests);
					if (parent_result.state != ChatPublicationState::Committed)
					{
						task->result.delegation_state = parent_result.state == ChatPublicationState::Uncertain ? ManagedInitialDelegationState::Uncertain : ManagedInitialDelegationState::Rejected;
						task->result.error = parent_result.state == ChatPublicationState::Uncertain ? "The request's save could not be confirmed. No agent was started." : "The request was not recorded. No agent was started.";
						if (parent_result.state == ChatPublicationState::Uncertain)
						{
							task->uncertain_root = true;
							task->phase = PendingManagedInitialMention::Phase::ReconcilingRoot;
							task->retry_at = InitialClock::now();
							try
							{
								(void)task->completion(task->result);
							}
							catch (...)
							{
								diagnostics::Write("Managed uncertain mention completion failed.");
							}
							task->completion_settled = true;
							changed = true;
						}
						else
							CleanInitialCreation(*task);
						continue;
					}
					auto* live = ChatDomainService().FindChatById(app, id);
					// The guard ran in this same serialized owner stack; the original volatile owner remains exact.
					committed_parent.metadata_incarnation = task->incarnation;
					committed_parent.managed_subagent_revision = std::max(committed_parent.managed_subagent_revision, live->managed_subagent_revision);
					*live = committed_parent;
					task->root_birth = live->managed_followup_incarnation;
					task->result.input_recorded = true;
					task->result.parent_message = live->messages.back();
					task->result.parent_message_index = live->messages.size() - 1;
					work->before = *live;
					task->metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *live);
					ChatSession committed_transcript = work->transcript;
					const auto transcript_result = ChatRepository::PublishPreparedChatPublication(work->transcript_prepared, owned, &committed_transcript);
					if (transcript_result.state != ChatPublicationState::Committed)
					{
						task->result.delegation_state = transcript_result.state == ChatPublicationState::Uncertain ? ManagedInitialDelegationState::Uncertain : ManagedInitialDelegationState::Rejected;
						task->result.error = "Your request was recorded, but its managed conversation could not be confirmed. No agent was started.";
						CleanInitialCreation(*task);
						changed = true;
						continue;
					}
					// The initial ledger is LAST. No live child, queue or execution authority exists yet.
					const auto ledger_result = AgentRunLedger::PublishPreparedSave(work->ledger_prepared, owned);
					if (ledger_result.state != AgentRunPublishState::Committed)
					{
						task->result.delegation_state = ledger_result.state == AgentRunPublishState::Uncertain ? ManagedInitialDelegationState::Uncertain : ManagedInitialDelegationState::Rejected;
						task->result.error = "Your request was recorded, but delegation could not be confirmed. No agent was started.";
						CleanInitialCreation(*task);
						changed = true;
						continue;
					}
					task->accepted_run_id = work->run.id;
					task->accepted = true;
					task->result.delegation_state = ManagedInitialDelegationState::Committed;
					task->result.run_id = work->run.id;
					task->result.transcript_chat_id = work->run.transcript_chat_id;
					app.agent_runs.push_back(work->run);
					app.chats.push_back(std::move(committed_transcript));
					const auto& live_transcript = app.chats.back();
					task->accepted_transcript_incarnation = live_transcript.metadata_incarnation;
					task->accepted_transcript_created = live_transcript.created_at;
					task->accepted_transcript_birth = live_transcript.managed_followup_incarnation;
					auto owner = std::make_shared<ManagedInitialRuntimeOwner>();
					owner->root = task->root;
					owner->root_id = id;
					owner->transcript_id = task->transcript_id;
					owner->root_incarnation = task->incarnation;
					owner->transcript_incarnation = live_transcript.metadata_incarnation;
					owner->root_birth = work->run.managed_continuation->owner.root_birth_id;
					owner->transcript_birth = work->run.managed_continuation->owner.transcript_birth_id;
					owner->root_created = work->before.created_at;
					owner->transcript_created = live_transcript.created_at;
					owner->root_policy = InitialChildPolicy(*ChatDomainService().FindChatById(app, id));
					owner->root_settings = InitialConfiguration(app, *ChatDomainService().FindChatById(app, id));
					owner->expected = work->run;
					owner->settings = InitialConfiguration(app, live_transcript);
					owner->child_policy = InitialChildPolicy(live_transcript);
					owner->parent_stage_hook_for_tests = task->parent_stage_hook_for_tests;
					app.managed_initial_runtime_owners.emplace(work->run.id, owner);
					if (auto* root = ChatDomainService().FindChatById(app, id))
						++root->managed_subagent_revision;
					CleanInitialCreation(*task);
					changed = true;
				}
				if (task->phase == PendingManagedInitialMention::Phase::ReconcilingRoot)
				{
					const auto work = task->work;
					if (!work || !work->finished.load(std::memory_order_acquire))
						continue;
					const auto* root = ChatDomainService().FindChatById(app, task->root_id);
					if (app.managed_initial_shutdown)
					{
						CleanInitialCreation(*task);
						continue;
					}
					if (app.data_root != task->root || !root || root->metadata_incarnation != task->incarnation || root->created_at != work->before.created_at)
					{
						task->uncertain_root = false;
						CleanInitialCreation(*task);
						continue;
					} // No writes into a replacement namespace.
					const auto owned_confirmation = [&]
					{
						const auto* current = ChatDomainService().FindChatById(app, task->root_id);
						return !app.managed_initial_shutdown && app.data_root == task->root && current && current->metadata_incarnation == task->incarnation && current->messages == work->before.messages && ChatRepository::ChatMetadataFingerprint(app.data_root, *current) == task->metadata && InitialRootIdle(app, *current, true, nullptr, true);
					};
					if (!owned_confirmation())
						continue; // Retain the real ticket/evidence; metadata changes grant no overwrite authority.
					if (!task->confirmation_preparing)
					{
						if (InitialClock::now() < task->retry_at)
							continue;
						task->confirmation_preparing = true;
						StartInitialPreparation(work,
						                        [](InitialPreparation& work)
						                        {
							                        work.root_prepared.reset();
							                        work.transcript_prepared.reset();
							                        work.ledger_prepared.reset();
							                        work.error.clear();
							                        work.root_before_observed = false;
							                        const auto revision = ChatRepository::ChatStorageRevision(work.root, work.before.id);
							                        const auto saved = ChatRepository::LoadLocalChat(work.root, work.before.id, true, nullptr, false);
							                        if (revision && *revision == "missing" && work.root_was_absent)
							                        {
								                        work.root_before_observed = true;
								                        return;
							                        }
							                        if (revision && saved && InitialStoredChatMatches(work.root, *saved, work.before) && ChatRepository::ChatStorageRevision(work.root, work.before.id) == revision)
							                        {
								                        work.root_before_observed = true;
								                        return;
							                        }
							                        if (!revision || !saved || !InitialStoredChatMatches(work.root, *saved, work.parent) || ChatRepository::ChatStorageRevision(work.root, work.before.id) != revision)
							                        {
								                        work.error = "The unconfirmed parent has a different saved record. Its ticket and evidence were retained.";
								                        return;
							                        }
							                        work.root_prepared = ChatRepository::PrepareChatPublication(work.root, work.parent, ChatPublicationMode::UpdateExisting, *revision, &work.error);
						                        });
						continue;
					}
					task->confirmation_preparing = false;
					task->retry_at = InitialClock::now() + std::chrono::milliseconds(500);
					if (work->root_before_observed)
					{
						task->uncertain_root = false;
						CleanInitialCreation(*task);
						changed = true;
						continue;
					}
					if (work->root_prepared && work->error.empty())
					{
						ChatSession committed = work->parent;
						const auto confirmation = ChatRepository::PublishPreparedChatPublication(work->root_prepared, owned_confirmation, &committed);
						if (confirmation.state == ChatPublicationState::Committed)
						{
							committed.metadata_incarnation = task->incarnation;
							auto* current = ChatDomainService().FindChatById(app, task->root_id);
							committed.managed_subagent_revision = std::max(committed.managed_subagent_revision, current->managed_subagent_revision);
							*current = std::move(committed);
							task->root_birth = ChatDomainService().FindChatById(app, task->root_id)->managed_followup_incarnation;
							task->uncertain_root = false;
							CleanInitialCreation(*task);
							changed = true;
						}
					}
					continue; // Confirmation is bookkeeping only: this ticket can never admit a child.
				}
				if (task->phase != PendingManagedInitialMention::Phase::Cleaning || !task->retired->load(std::memory_order_acquire))
					continue;
				if (task->uncertain_root && app.managed_initial_shutdown)
				{
					task->phase = PendingManagedInitialMention::Phase::UncertainShutdown;
					continue;
				}
				task->phase = PendingManagedInitialMention::Phase::Callback;
				bool acked = false;
				if (!task->completion_settled)
				{
					try
					{
						acked = task->completion(task->result);
					}
					catch (...)
					{
						diagnostics::Write("Managed mention completion failed.");
					}
					task->completion_settled = true;
				}
				if (task->accepted)
				{
					const auto accepted = app.managed_initial_runtime_owners.find(task->accepted_run_id);
					if (accepted == app.managed_initial_runtime_owners.end())
					{
						app.managed_initial_mentions.erase(id);
						changed = true;
						continue;
					}
					auto owner = accepted->second;
					owner->completion_acked = acked;
					if (acked && !task->cancelled && !app.managed_initial_shutdown && !owner->input_forbidden && owner->expected.status == "queued" && !app.pending_agent_run_cancellation_ids.contains(owner->expected.id) && InitialRuntimeOwned(app, *owner))
						app.queued_agent_run_ids.push_back(owner->expected.id);
					else if (owner->desired_status.empty())
						ForbidInitialInput(*owner, "The recorded delegation lost its completion owner. The task was not started.");
				}
				app.managed_initial_mentions.erase(id);
				changed = true;
			}
			return changed;
		}
	} // namespace

	bool AgentRunScheduler::InitialSendAllowed(const AppState& app, const ChatSession& chat, const std::string& text)
	{
		if (HasPendingInitialMention(app, chat.id))
			return false;
		if (!InitialNeedsOwner(app, chat))
			return true;
		const auto owner = InitialOwner(app, chat);
		return owner && owner->completion_acked && owner->send_authorized && !owner->input_forbidden && !owner->input_written && owner->serial == 0 && owner->phase == ManagedInitialRuntimeOwner::Phase::Starting && text == InitialTask(*owner) && InitialRuntimeOwned(app, *owner);
	}

	bool AgentRunScheduler::InitialLaunchAllowed(const AppState& app, const AcpSessionState& session, const ChatSession& chat, std::string* error_out)
	{
		if (!InitialNeedsOwner(app, chat))
			return true;
		const auto owner = InitialOwner(app, chat);
		if (session.running)
			return owner && session.managed_initial_owner == owner && owner->session == &session && owner->process == session.last_process_id && InitialSessionOwned(app, session, chat);
		const bool allowed = owner && session.managed_initial_owner == owner && owner->session == &session && owner->completion_acked && !owner->input_forbidden && !owner->input_written && (owner->phase == ManagedInitialRuntimeOwner::Phase::Starting || owner->phase == ManagedInitialRuntimeOwner::Phase::Row || owner->phase == ManagedInitialRuntimeOwner::Phase::Input) && owner->expected.status == "running" && InitialCurrentTurn(*owner).dispatch_attempted && !session.managed_launch_attempted && !session.running && (owner->resume_only ? (owner->expected.managed_continuation->execution && chat.native_session_id == owner->expected.managed_continuation->execution->native_session_id && (session.session_id.empty() || session.session_id == chat.native_session_id)) : (session.session_id.empty() && chat.native_session_id.empty())) && InitialRuntimeOwned(app, *owner);
		if (!allowed && error_out)
			*error_out = "This managed initial input has no current launch permission. It was not replayed.";
		return allowed;
	}

	bool AgentRunScheduler::PrepareInitialSetupRow(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		if (!InitialNeedsOwner(app, chat)) return !session.managed_initial_owner;
		const auto owner = InitialOwner(app, chat);
		const auto refuse = [&]()
		{
			if (owner && session.managed_initial_owner == owner && owner->session == &session)
			{
				ForbidInitialInput(*owner, "The managed setup lost its exact queued task, captured agent policy or admitted row. No task input was sent.");
				session.last_error = owner->diagnostic;
			}
			return false;
		};
		if (!owner || session.managed_initial_owner != owner || owner->session != &session || FindAcpSessionForChat(app, chat.id) != &session || ChatDomainService().FindChatById(app, chat.id) != &chat || !InitialRuntimeOwned(app, *owner) || !owner->completion_acked || owner->input_forbidden || owner->input_attempted || owner->input_written || owner->settled || owner->expected.status != "running" || !session.running || !session.initialized || session.session_ready || session.session_setup_request_id != 0 || !session.managed_launch_attempted || session.managed_agent_run_id != owner->expected.id || session.provider_id != owner->expected.provider_id || session.process_execution_host_id != chat.execution_host_id || owner->process.empty() || session.last_process_id != owner->process || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed) return refuse();
		const auto& managed = owner->expected.managed_continuation;
		if (!managed || managed->turns.empty() || managed->current_turn_id != managed->turns.back().turn_id || managed->current_turn_number != managed->turns.size() || managed->owner.delegation_grant) return refuse();
		const auto& turn = managed->turns.back();
		if (!turn.dispatch_attempted || turn.principal.kind != ManagedRunPrincipalKind::UserRoot || turn.principal.root_chat_id != owner->root_id || turn.principal.root_birth_id != owner->root_birth || !turn.principal.parent_run_id.empty() || !turn.principal.parent_transcript_birth_id.empty()) return refuse();
		const auto admitted = [&]()
		{
			if (owner->serial <= 0 || session.turn_serial != owner->serial || (owner->phase != ManagedInitialRuntimeOwner::Phase::Row && owner->phase != ManagedInitialRuntimeOwner::Phase::Input) || session.turn_first_user_message_index != owner->row || session.turn_user_message_index != owner->row || owner->row < 0 || static_cast<std::size_t>(owner->row) >= chat.messages.size()) return false;
			const auto& row = chat.messages[owner->row];
			return row.role == MessageRole::User && row.content == InitialTask(*owner) && row.created_at == owner->row_created && !row.acp_prompt_discarded && row.attachments.empty() && row.markdown_store_files.empty() && row.markdown_store_prompt_blocks.empty();
		};
		if (owner->serial > 0) return admitted() ? true : refuse();
		if (AcpSessionIsWaitingForInput(session)) return false; // Preserve a genuine pre-setup question or permission wait.
		if (owner->phase != ManagedInitialRuntimeOwner::Phase::Starting || session.turn_serial != 0 || owner->row != -1 || session.turn_first_user_message_index != -1 || session.turn_user_message_index != -1 || session.processing || !session.queued_prompt.empty() || session.queued_user_prompts.size() != 1 || chat.acp_queued_prompts.size() != 1 || chat.acp_dispatched_queued_prompt_count != 0) return refuse();
		const auto& queued = session.queued_user_prompts.front();
		const auto& run = owner->expected;
		std::string capability = strings::NonEmptyOrFallback(run.execution_capability, AgentDefinitionService::ExecutionCapabilityForProvider(run.provider_id));
		if (chat.execution_host_id != execution_hosts::kLocalHostId) capability = "uam-prompt-injected";
		if (app.settings.central_provider_configuration.enabled) capability = "uam-central-native-setup";
		if (queued != chat.acp_queued_prompts.front() || queued.text != InitialTask(*owner) || !queued.direct_admission || !queued.append_user_message || queued.existing_user_message_index != -1 || !queued.existing_user_message_created_at.empty() || queued.id.empty() || queued.revision <= 0 || !queued.attachments.empty() || !queued.markdown_store_files.empty() || !queued.markdown_store_prompt_blocks.empty() || queued.goal_mode || !queued.goal_id.empty() || queued.computer_use_mode || !queued.claude_quota_continuation_id.empty() || queued.uam_agent_id != run.agent_id || queued.uam_agent_definition_hash != run.definition_hash || queued.uam_agent_definition_snapshot != run.definition_snapshot || queued.uam_agent_instructions != run.definition_instructions || queued.uam_agent_skills != run.skills_snapshot || queued.uam_agent_delegates != run.delegates_snapshot || queued.uam_agent_workspace_access != run.effective_workspace_access || queued.uam_agent_execution_capability != capability || session.active_uam_agent_id != queued.uam_agent_id || session.active_uam_agent_definition_hash != queued.uam_agent_definition_hash || session.active_uam_agent_definition_snapshot != queued.uam_agent_definition_snapshot || session.active_uam_agent_instructions != queued.uam_agent_instructions || session.active_uam_agent_skills != queued.uam_agent_skills || session.active_uam_agent_delegates != queued.uam_agent_delegates || session.active_uam_agent_workspace_access != queued.uam_agent_workspace_access || session.active_uam_agent_execution_capability != queued.uam_agent_execution_capability) return refuse();
		if (owner->resume_only)
		{
			if (!managed->execution || !queued.managed_turn || *queued.managed_turn != ManagedRunPromptIdentity{run.id, turn.turn_id, turn.turn_number, managed->execution->native_session_id, ManagedRunContinuity::Native}) return refuse();
		}
		else if (queued.managed_turn) return refuse();
		// Use the ordinary queue's save/admission barriers and append the real authored row.
		// An initialized but unready session cannot deliver input at the end of that path.
		if (!DrainNextQueuedAcpUserPrompt(app, session, chat)) return false;
		return InitialRuntimeOwned(app, *owner) && admitted() ? true : refuse();
	}

	bool AgentRunScheduler::InitialRowAdmitted(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
	{
		if (!InitialNeedsOwner(app, chat))
			return true;
		const auto owner = InitialOwner(app, chat);
		if (!owner || session.managed_initial_owner != owner || owner->session != &session || owner->serial != 0 || !InitialRuntimeOwned(app, *owner) || session.turn_serial <= 0 || session.turn_first_user_message_index < 0 || session.turn_first_user_message_index != session.turn_user_message_index || static_cast<std::size_t>(session.turn_user_message_index) >= chat.messages.size())
		{
			if (owner)
				ForbidInitialInput(*owner);
			if (error_out)
				*error_out = "The managed task did not retain its exact admitted row.";
			return false;
		}
		const auto& row = chat.messages[session.turn_user_message_index];
		if (row.role != MessageRole::User || row.content != InitialTask(*owner) || row.created_at.empty() || !row.attachments.empty() || row.acp_prompt_discarded)
		{
			ForbidInitialInput(*owner);
			return false;
		}
		owner->serial = session.turn_serial;
		owner->row = session.turn_user_message_index;
		owner->row_created = row.created_at;
		if (session.running && session.managed_launch_attempted)
			owner->process = session.last_process_id;
		owner->phase = ManagedInitialRuntimeOwner::Phase::Row;
		owner->send_authorized = false;
		return true; // Poll performs conditional receipt checkpoint; input remains held until Committed.
	}

	bool AgentRunScheduler::InitialSessionOwned(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		if (!InitialNeedsOwner(app, chat))
			return true;
		const auto owner = InitialOwner(app, chat);
		if (!owner || session.managed_initial_owner != owner || owner->session != &session || !owner->completion_acked || app.managed_initial_shutdown)
			return false;
		if (!owner->input_forbidden)
			return InitialRuntimeOwned(app, *owner);
		// Retain only the genuinely reaped original lease for idle runtime inspection.
		// Input/launch guards stay forbidden; repeating Stop here would rewrite its canonical save.
		return InitialReadOnlyStoppedLeaseOwned(app, *owner, session, chat);
	}

	void AgentRunScheduler::InitialProcessStarted(AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		const auto owner = InitialOwner(app, chat);
		if (!owner)
			return;
		if (session.managed_initial_owner != owner || owner->session != &session || !session.running || !session.managed_launch_attempted || session.last_process_id.empty() || !owner->process.empty() || !InitialRuntimeOwned(app, *owner))
		{
			ForbidInitialInput(*owner);
			return;
		}
		owner->process = session.last_process_id;
	}

	std::optional<ManagedNativeReadScope> AgentRunScheduler::ManagedReadOnlyNativeScope(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		const auto owner = InitialOwner(app, chat);
		if (!owner || session.managed_initial_owner != owner || owner->session != &session || FindAcpSessionForChat(app, chat.id) != &session || !InitialRuntimeOwned(app, *owner) || !session.running || !session.managed_launch_attempted || session.provider_id != "copilot-cli" || session.protocol_kind != "copilot-sdk" || session.process_execution_host_id != "local" || session.managed_agent_run_id != owner->expected.id || owner->process.empty() || session.last_process_id != owner->process || owner->serial <= 0 || session.turn_serial != owner->serial || !owner->completion_acked || owner->input_forbidden || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending || owner->expected.status != "running") return std::nullopt;
		if (session.turn_user_message_index != owner->row || session.turn_first_user_message_index != owner->row || owner->row < 0 || static_cast<std::size_t>(owner->row) >= chat.messages.size()) return std::nullopt;
		const auto& row = chat.messages[owner->row];
		if (row.role != MessageRole::User || row.content != InitialTask(*owner) || row.created_at != owner->row_created || row.acp_prompt_discarded || !row.attachments.empty() || !row.markdown_store_files.empty() || !row.markdown_store_prompt_blocks.empty()) return std::nullopt;
		const auto& run = owner->expected;
		const auto& managed = run.managed_continuation;
		const auto* root = ChatDomainService().FindChatById(app, owner->root_id);
		const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const auto* provider = ProviderResolutionService().ProviderForChat(app, chat);
		if (!managed || managed->turns.empty() || managed->turns.size() > 33 || managed->current_turn_id != managed->turns.back().turn_id || managed->current_turn_number != managed->turns.size() || managed->owner.delegation_grant || run.provider_id != "copilot-cli" || run.depth != 1 || !run.parent_run_id.empty() || !run.resumed_from_run_id.empty() || !run.goal_id.empty() || run.effective_workspace_access != "read" || !root || !root->active_goal_id.empty() || !root->goal_owner_chat_id.empty() || !root->temporary_parent_chat_id.empty() || !host || host->id != "local" || host->transport != "local" || !provider || provider->id != run.provider_id || provider->structured_protocol != "copilot-sdk" || !provider->supports_structured || !provider->supports_resume || !ProviderResolutionService().ChatProviderIsAvailable(app, chat) || chat.execution_host_id != "local" || chat.routing_mode != "explicit" || !paths::IsControllerLocalWorkspace(chat) || chat.small_model_mode || !chat.active_goal_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.temporary_parent_chat_id.empty() || chat.approval_mode != approval_modes::kPlanApprovalMode || chat.model_id.empty() || chat.model_id.size() > 160 || chat.model_id.find('\0') != std::string::npos || strings::ToLowerAscii(chat.model_id) == "auto" || strings::ToLowerAscii(chat.model_id) == "default" || chat.model_id != run.model_id || chat.workspace_directory.empty() || chat.workspace_directory.size() > 4096 || chat.workspace_directory.find('\0') != std::string::npos || !std::filesystem::path(chat.workspace_directory).is_absolute()) return std::nullopt;
		if (!std::ranges::all_of(managed->turns, [&](const auto& turn) { return turn.principal.kind == ManagedRunPrincipalKind::UserRoot && turn.principal.root_chat_id == run.root_chat_id && turn.principal.root_birth_id == managed->owner.root_birth_id && turn.principal.parent_run_id.empty() && turn.principal.parent_transcript_birth_id.empty(); })) return std::nullopt;
		if ((owner->resume_only && (!FollowupShape(run) || !managed->execution)) || (!owner->resume_only && (managed->turns.size() != 1 || managed->execution))) return std::nullopt;
		ManagedNativeReadScope scope;
		scope.run_id = run.id; scope.turn_id = managed->current_turn_id; scope.process_id = owner->process;
		scope.root_birth_id = owner->root_birth; scope.transcript_birth_id = owner->transcript_birth;
		scope.model_id = chat.model_id; scope.workspace_directory = chat.workspace_directory;
		scope.native_session_id = owner->resume_only ? managed->execution->native_session_id : std::string{};
		scope.configuration = InitialConfiguration(app, chat); scope.child_policy = InitialChildPolicy(chat);
		scope.turn_serial = owner->serial; scope.resume_only = owner->resume_only;
		if (scope.configuration.empty() || scope.child_policy.empty() || scope.configuration.size() > 256U * 1024U || scope.child_policy.size() > 256U * 1024U) return std::nullopt;
		return scope;
	}

	bool AgentRunScheduler::ManagedReadPolicyRetained(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const ManagedNativeReadScope& scope)
	{
		const auto owner = ImmutableManagedReadOwner(app, session, chat, scope);
		if (!owner || owner->binding_withheld || !InitialRuntimeOwned(app, *owner, false) || (!owner->settled && owner->input_forbidden) || (owner->settled && owner->desired_status != "completed")) return false;
		const auto* root = ChatDomainService().FindChatById(app, owner->root_id);
		// Retain only an already observed transaction through its ordinary save/stop;
		// this does not issue a new generation or release an input boundary.
		return root && root->active_goal_id.empty() && root->goal_owner_chat_id.empty() && root->temporary_parent_chat_id.empty() && owner->setup_execution_observed && owner->native_response_seen && owner->dispatch_configuration && (scope.resume_only || owner->observation_generation) && owner->dispatch_settings == scope.configuration && owner->child_policy == scope.child_policy && InitialConfiguration(app, chat) == scope.configuration && InitialChildPolicy(chat) == scope.child_policy && chat.model_id == scope.model_id && chat.workspace_directory == scope.workspace_directory && chat.approval_mode == approval_modes::kPlanApprovalMode && owner->observed_native == session.session_id && owner->observed_native == chat.native_session_id && owner->observed_native == owner->native_observed && owner->observed_model == scope.model_id && owner->observed_workspace == scope.workspace_directory && owner->observed_mode == approval_modes::kPlanApprovalMode && (!scope.resume_only || owner->observed_native == scope.native_session_id);
	}

	void AgentRunScheduler::RejectManagedReadPolicy(AppState& app, AcpSessionState& session, const ChatSession& chat, const ManagedNativeReadScope& scope, std::string_view diagnostic)
	{
		const auto owner = ImmutableManagedReadOwner(app, session, chat, scope);
		if (!owner) return;
		owner->binding_withheld = true;
		if (owner->resume_only)
		{
			ForbidInitialInput(*owner, diagnostic.empty() ? "The provider did not confirm the saved managed read policy. No input was replayed." : std::string(diagnostic.substr(0, 1024)));
			session.last_error = owner->diagnostic;
		}
	}

	bool AgentRunScheduler::ManagedReadPolicyObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, const ManagedNativeReadScope& scope, std::string_view native, std::string_view model, std::string_view workspace, std::string_view mode)
	{
		const auto current = ManagedReadOnlyNativeScope(app, session, chat);
		if (!current || *current != scope)
		{
			RejectManagedReadPolicy(app, session, chat, scope, "The managed native read-policy scope changed before observation. No input was replayed.");
			return false;
		}
		const auto owner = session.managed_initial_owner;
		const bool exact = !owner->binding_withheld && !owner->settled && uuid::IsCanonicalUuid(native) && native == session.session_id && native == chat.native_session_id && model == scope.model_id && workspace == scope.workspace_directory && mode == approval_modes::kPlanApprovalMode && owner->native_response_seen && owner->native_observed == native;
		const bool ready = owner->resume_only ? (!owner->input_attempted && !owner->input_written && native == scope.native_session_id && owner->dispatch_configuration && owner->dispatch_configuration->native_session_id == native) : (owner->input_attempted && owner->input_written && owner->configuration_captured && owner->dispatch_configuration && owner->dispatch_settings == scope.configuration && owner->child_policy == scope.child_policy && owner->dispatch_configuration->effective_model_id == model && owner->dispatch_configuration->workspace_directory == workspace);
		if (!exact || !ready || (owner->setup_execution_observed && (owner->observed_native != native || owner->observed_model != model || owner->observed_workspace != workspace || owner->observed_mode != mode)))
		{
			RejectManagedReadPolicy(app, session, chat, scope, "The provider contradicted the exact managed native model, workspace, Plan mode or read policy. No input was replayed.");
			return false;
		}
		owner->setup_execution_observed = true;
		owner->observed_native = native; owner->observed_model = model;
		owner->observed_workspace = workspace; owner->observed_mode = mode;
		if (!owner->resume_only && !owner->observation_generation) owner->observation_generation = 1;
		return true; // Only an ordinary subsequent chat save can record this generation.
	}

	void AgentRunScheduler::InitialExecutionObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view native, std::string_view model, std::string_view native_mode, bool complete_init)
	{
		const auto owner = session.managed_initial_owner;
		if (owner && owner->resume_only)
		{
			const auto& binding = *owner->expected.managed_continuation->execution;
			const auto mode = approval_modes::EffectiveProviderMode(binding.approval_mode, binding.command_safety_tier);
			const bool scoped = owner->session == &session && session.managed_initial_owner == owner && owner->input_written && !owner->process.empty() && owner->process == session.last_process_id && owner->serial == session.turn_serial && InitialRuntimeOwned(app, *owner);
			const bool contradictory = complete_init ? (native != binding.native_session_id || model != binding.effective_model_id || native_mode != mode) : ((!native.empty() && native != binding.native_session_id) || (!model.empty() && model != binding.effective_model_id) || (!native_mode.empty() && native_mode != mode));
			if (scoped && !contradictory && complete_init) owner->native_response_seen = true;
			if (!scoped || contradictory)
			{
				owner->input_forbidden = true;
				if (!IsTerminal(owner->expected.status))
				{
					owner->desired_status = "failed"; owner->diagnostic_code = "native_resume_observation_changed";
					owner->diagnostic = "The provider contradicted the saved managed model, mode or native identity. Input was not replayed.";
				}
			}
			return;
		}
		if (!owner || owner->binding_withheld)
			return;
		// Only the original actual one-shot attempt can contribute observation evidence.
		if (owner->session != &session || InitialOwner(app, chat) != owner || FindAcpSessionForChat(app, chat.id) != &session || !InitialRuntimeOwned(app, *owner) || !owner->input_written || !session.running || owner->process.empty() || session.last_process_id != owner->process || session.turn_serial != owner->serial || session.turn_user_message_index != owner->row || session.turn_first_user_message_index != owner->row || owner->row < 0 || static_cast<std::size_t>(owner->row) >= chat.messages.size() || chat.messages[owner->row].role != MessageRole::User || chat.messages[owner->row].content != InitialTask(*owner) || chat.messages[owner->row].created_at != owner->row_created || chat.messages[owner->row].acp_prompt_discarded || !session.managed_launch_attempted || session.managed_agent_run_id != owner->expected.id || session.provider_id != "claude-cli" || session.protocol_kind != "claude-code-stream-json" || session.process_execution_host_id != "local" || !owner->dispatch_configuration)
		{
			owner->binding_withheld = true;
			return;
		}
		if (!complete_init)
		{
			// A result cannot fill omitted init fields or refresh contradictory proof.
			if ((!native.empty() && native != owner->observed_native) || (!model.empty() && model != owner->observed_model) || (!native_mode.empty() && native_mode != owner->observed_mode))
				owner->binding_withheld = true;
			return;
		}
		if (!uuid::IsCanonicalUuid(native) || model.empty() || model.size() > 160 || native_mode.empty() || native_mode.size() > 64 || model != owner->expected.model_id || model != owner->dispatch_configuration->effective_model_id || (!owner->observed_native.empty() && (native != owner->observed_native || model != owner->observed_model || native_mode != owner->observed_mode)))
		{
			owner->binding_withheld = true;
			return;
		}
		if (!owner->observation_generation && owner->settled)
		{
			owner->binding_withheld = true;
			return;
		}
		if (!owner->observation_generation)
		{
			owner->observed_native = native;
			owner->observed_model = model;
			owner->observed_mode = native_mode;
			owner->observation_generation = 1;
		}
	}

	void AgentRunScheduler::InitialSetupExecutionObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view request_id, std::string_view native, std::string_view model, std::string_view workspace, bool read_only_openai_policy_confirmed)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->resume_only || owner->binding_withheld || session.provider_id != "codex-cli" || session.protocol_kind != "codex-app-server") return;
		const bool owned = owner->session == &session && InitialOwner(app, chat) == owner && FindAcpSessionForChat(app, chat.id) == &session && InitialRuntimeOwned(app, *owner) && session.running && session.managed_launch_attempted && session.managed_agent_run_id == owner->expected.id && session.process_execution_host_id == "local" && !owner->process.empty() && session.last_process_id == owner->process && session.session_id.empty() && chat.native_session_id.empty() && session.session_setup_request_id > 0 && request_id == std::to_string(session.session_setup_request_id) && !owner->input_attempted && !owner->input_written && !owner->settled && !owner->input_forbidden && owner->completion_acked && owner->expected.status == "running" && (owner->phase == ManagedInitialRuntimeOwner::Phase::Starting || owner->phase == ManagedInitialRuntimeOwner::Phase::Row || owner->phase == ManagedInitialRuntimeOwner::Phase::Input);
		if (!owned || !read_only_openai_policy_confirmed || !uuid::IsCanonicalUuid(native) || model.empty() || model.size() > 160 || strings::ToLowerAscii(std::string(model)) == "auto" || strings::ToLowerAscii(std::string(model)) == "default" || model != owner->expected.model_id || workspace.empty() || workspace.size() > 4096 || !std::filesystem::path(std::string(workspace)).is_absolute() || workspace != chat.workspace_directory || chat.execution_host_id != "local" || chat.routing_mode != "explicit" || owner->expected.provider_id != "codex-cli" || owner->expected.depth != 1 || !owner->expected.parent_run_id.empty() || owner->expected.effective_workspace_access != "read" || !owner->expected.managed_continuation || owner->expected.managed_continuation->owner.delegation_grant || owner->expected.managed_continuation->turns.size() != 1 || owner->expected.managed_continuation->turns.front().principal.kind != ManagedRunPrincipalKind::UserRoot || (owner->setup_execution_observed && (native != owner->observed_native || model != owner->observed_model || workspace != owner->observed_workspace)))
		{
			owner->binding_withheld = true;
			return;
		}
		owner->setup_execution_observed = true;
		owner->observed_native = native;
		owner->observed_model = model;
		owner->observed_workspace = workspace;
		// No generation/save/configuration proof is minted before the real final input boundary.
	}

	void AgentRunScheduler::InitialGeminiExecutionObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view request_id, std::string_view native, std::string_view model, std::string_view mode)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->session != &session || owner->expected.provider_id != "gemini-cli" || owner->resume_only || owner->binding_withheld) return;
		const auto& run = owner->expected;
		const auto& managed = run.managed_continuation;
		const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const bool owned = InitialOwner(app, chat) == owner && FindAcpSessionForChat(app, chat.id) == &session && InitialRuntimeOwned(app, *owner) && session.provider_id == run.provider_id && session.protocol_kind == "gemini-acp" && session.running && session.initialized && session.managed_launch_attempted && session.managed_agent_run_id == run.id && session.process_execution_host_id == "local" && host && host->id == "local" && host->transport == "local" && !owner->process.empty() && session.last_process_id == owner->process && session.session_id.empty() && chat.native_session_id.empty() && session.session_setup_request_id > 0 && request_id == std::to_string(session.session_setup_request_id) && owner->completion_acked && !owner->input_attempted && !owner->input_written && !owner->input_forbidden && !owner->settled && run.status == "running";
		const bool shape = managed && !managed->execution && !managed->owner.delegation_grant && managed->turns.size() == 1 && managed->current_turn_id == run.id && managed->current_turn_number == 1 && managed->turns.front().dispatch_attempted && managed->turns.front().principal.kind == ManagedRunPrincipalKind::UserRoot && managed->turns.front().principal.root_chat_id == owner->root_id && managed->turns.front().principal.root_birth_id == owner->root_birth && managed->turns.front().principal.parent_run_id.empty() && managed->turns.front().principal.parent_transcript_birth_id.empty() && run.depth == 1 && run.parent_run_id.empty() && run.resumed_from_run_id.empty() && run.goal_id.empty() && run.effective_workspace_access == "read";
		const bool exact = uuid::IsCanonicalUuid(native) && !model.empty() && model.size() <= 160 && strings::TrimAsciiView(model) == model && std::ranges::none_of(model, [](unsigned char ch) { return ch < 32 || ch == 127; }) && strings::ToLowerAscii(std::string(model)) != "auto" && strings::ToLowerAscii(std::string(model)) != "default" && model == run.model_id && model == chat.model_id && mode == approval_modes::kPlanApprovalMode && chat.approval_mode == approval_modes::kPlanApprovalMode && chat.routing_mode == "explicit" && !chat.small_model_mode && paths::IsControllerLocalWorkspace(chat) && !chat.workspace_directory.empty() && chat.workspace_directory.size() <= 4096 && chat.workspace_directory.find('\0') == std::string::npos && std::filesystem::path(chat.workspace_directory).is_absolute();
		if (!owned || !shape || !exact || owner->setup_execution_observed)
		{
			// Unsupported or contradictory native proof only withholds the binding;
			// the existing truthful initial one-shot retains its own input policy.
			owner->binding_withheld = true;
			return;
		}
		owner->setup_execution_observed = true;
		owner->observed_native = native;
		owner->observed_model = model;
		owner->observed_mode = mode;
		owner->observed_workspace = chat.workspace_directory; // Exact owned launch/setup scope, not a native cwd claim.
		owner->observed_provider_version = session.agent_version; // Adapter qualified this owned initialize observation.
		// Actual input, completed-model telemetry, a later save and completed/reaped publication remain required.
	}

	void AgentRunScheduler::InitialGeminiResultObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view request_id, std::string_view model)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->session != &session || owner->expected.provider_id != "gemini-cli" || owner->resume_only || owner->binding_withheld) return;
		const bool owned = InitialOwner(app, chat) == owner && FindAcpSessionForChat(app, chat.id) == &session && InitialRuntimeOwned(app, *owner) && owner->completion_acked && owner->input_attempted && owner->input_written && !owner->input_forbidden && !owner->settled && owner->expected.status == "running" && session.running && session.processing && session.provider_id == "gemini-cli" && session.protocol_kind == "gemini-acp" && session.agent_name == "gemini-cli" && !owner->observed_provider_version.empty() && session.agent_version == owner->observed_provider_version && session.process_execution_host_id == "local" && !owner->process.empty() && session.last_process_id == owner->process && owner->serial > 0 && session.turn_serial == owner->serial && session.turn_user_message_index == owner->row && session.turn_first_user_message_index == owner->row && session.prompt_request_id > 0 && request_id == std::to_string(session.prompt_request_id);
		const bool row = owner->row >= 0 && static_cast<std::size_t>(owner->row) < chat.messages.size() && chat.messages[owner->row].role == MessageRole::User && chat.messages[owner->row].content == InitialTask(*owner) && chat.messages[owner->row].created_at == owner->row_created && !chat.messages[owner->row].acp_prompt_discarded;
		const bool exact = owner->setup_execution_observed && owner->configuration_captured && owner->dispatch_configuration && owner->native_response_seen && owner->native_observed == owner->observed_native && session.session_id == owner->observed_native && chat.native_session_id == owner->observed_native && !model.empty() && model == owner->observed_model && model == owner->dispatch_configuration->effective_model_id && model == session.current_model_id && owner->observed_mode == approval_modes::kPlanApprovalMode && session.current_mode_id == owner->observed_mode && InitialConfiguration(app, chat) == owner->dispatch_settings && InitialChildPolicy(chat) == owner->child_policy;
		if (!owned || !row || !exact || owner->observation_generation)
		{
			owner->binding_withheld = true;
			return;
		}
		// This is post-input native model telemetry, not the setup's configured-model echo.
		// An ordinary subsequent save must still persist this observed generation.
		owner->observation_generation = 1;
	}

	bool AgentRunScheduler::InitialResumeSetupExecutionObserved(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view request_id, std::string_view native, std::string_view model, std::string_view workspace, bool read_only_openai_policy_confirmed, std::string_view effective_mode, std::string_view collaboration_model, bool collaboration_supplied)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || !owner->resume_only) return true; // Ordinary and initial setup behavior remains unchanged.
		const bool response = !request_id.empty();
		const auto& value = owner->expected.managed_continuation;
		const auto* binding = value && value->execution ? &*value->execution : nullptr;
		const bool owned = owner->session == &session && InitialOwner(app, chat) == owner && FindAcpSessionForChat(app, chat.id) == &session && InitialRuntimeOwned(app, *owner) && FollowupShape(owner->expected) && session.provider_id == "codex-cli" && session.protocol_kind == "codex-app-server" && session.running && session.managed_launch_attempted && session.managed_agent_run_id == owner->expected.id && session.process_execution_host_id == "local" && !owner->process.empty() && session.last_process_id == owner->process && owner->completion_acked && owner->expected.status == "running";
		const bool correlated = response ? (session.session_setup_request_id > 0 && request_id == std::to_string(session.session_setup_request_id) && !owner->input_attempted && !owner->input_written && !owner->settled && !owner->input_forbidden && (owner->phase == ManagedInitialRuntimeOwner::Phase::Starting || owner->phase == ManagedInitialRuntimeOwner::Phase::Row || owner->phase == ManagedInitialRuntimeOwner::Phase::Input)) : (owner->setup_execution_observed && owner->native_response_seen);
		if (!owned || !correlated || !binding || owner->binding_withheld || !read_only_openai_policy_confirmed || !uuid::IsCanonicalUuid(native) || native != binding->native_session_id || session.session_id != native || chat.native_session_id != native || model.empty() || model.size() > 160 || model.find('\0') != std::string_view::npos || model != binding->effective_model_id || workspace.empty() || workspace.size() > 4096 || workspace.find('\0') != std::string_view::npos || workspace != binding->workspace_directory || (!response && !collaboration_supplied) || (collaboration_supplied && (effective_mode != approval_modes::kPlanApprovalMode || collaboration_model != binding->effective_model_id)))
		{
			owner->binding_withheld = true;
			ForbidInitialInput(*owner, "The provider did not confirm the exact saved managed Codex model, policy, mode or native conversation. No input was replayed.");
			session.last_error = owner->diagnostic;
			return false;
		}
		if (response)
		{
			owner->setup_execution_observed = true;
			owner->observed_native = native;
			owner->observed_model = model;
			owner->observed_workspace = workspace;
			owner->native_observed = native;
			owner->native_response_seen = true;
		}
		// Only raw effective reply/settings evidence can release the pre-input hold.
		// A later matching event never repairs a revoked lease or refreshes an input attempt.
		if (collaboration_supplied && !owner->input_attempted && !owner->input_forbidden && !owner->settled)
			owner->observed_mode = effective_mode;
		return true;
	}

	void AgentRunScheduler::InitialExecutionProofObserved(AcpSessionState& session, std::string_view native, std::string_view model, std::string_view workspace, bool supplied_fields_valid)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->session != &session || session.provider_id != "codex-cli" || session.protocol_kind != "codex-app-server" || owner->expected.provider_id != session.provider_id || session.process_execution_host_id != "local" || session.managed_agent_run_id != owner->expected.id || owner->process.empty() || owner->process != session.last_process_id) return;
		if (owner->resume_only)
		{
			const auto& value = owner->expected.managed_continuation;
			const auto* binding = value && value->execution ? &*value->execution : nullptr;
			if (!binding || !supplied_fields_valid || native.size() > 160 || model.size() > 160 || workspace.size() > 4096 || native.find('\0') != std::string_view::npos || model.find('\0') != std::string_view::npos || workspace.find('\0') != std::string_view::npos || (!native.empty() && native != binding->native_session_id) || (!model.empty() && model != binding->effective_model_id) || (!workspace.empty() && workspace != binding->workspace_directory))
			{
				owner->binding_withheld = true;
				ForbidInitialInput(*owner, "The provider contradicted the saved managed Codex execution evidence. No input was replayed.");
			}
			return; // Metadata can revoke resume proof, never create it or repair a withheld observation.
		}
		if (!supplied_fields_valid || native.size() > 160 || model.size() > 160 || workspace.size() > 4096 || native.find('\0') != std::string_view::npos || model.find('\0') != std::string_view::npos || workspace.find('\0') != std::string_view::npos || (owner->setup_execution_observed && ((!native.empty() && native != owner->observed_native) || (!model.empty() && model != owner->observed_model) || (!workspace.empty() && workspace != owner->observed_workspace))))
			owner->binding_withheld = true;
		// Never fill missing fields, advance a generation, or repair withheld proof.
	}

	void AgentRunScheduler::InitialChatSaved(AppState& app, const ChatSession& chat)
	{
		const auto owner = InitialOwner(app, chat);
		const auto* session = FindAcpSessionForChat(app, chat.id);
		if (!owner || !session || session->managed_initial_owner != owner || owner->session != session || !InitialRuntimeOwned(app, *owner, false))
			return;
		if (!chat.native_session_id.empty() && chat.native_session_id == session->session_id)
			owner->saved_native = chat.native_session_id;
		const int row = session->turn_user_message_index;
		if (session->turn_serial > 0 && row >= 0 && row == session->turn_first_user_message_index && static_cast<std::size_t>(row) < chat.messages.size() && chat.messages[row].role == MessageRole::User && chat.messages[row].content == InitialTask(*owner) && !chat.messages[row].acp_prompt_discarded)
		{
			owner->saved_serial = session->turn_serial;
			owner->saved_row = row;
			owner->saved_row_created = chat.messages[row].created_at;
		}
		if (!owner->binding_withheld && owner->observation_generation && owner->input_written && session->last_process_id == owner->process && session->turn_serial == owner->serial && row == owner->row && row >= 0 && static_cast<std::size_t>(row) < chat.messages.size() && chat.messages[row].role == MessageRole::User && chat.messages[row].content == InitialTask(*owner) && chat.messages[row].created_at == owner->row_created && !chat.messages[row].acp_prompt_discarded && owner->saved_serial == owner->serial && owner->saved_row == owner->row && owner->saved_row_created == owner->row_created && chat.native_session_id == owner->observed_native && session->session_id == owner->observed_native && InitialChildPolicy(chat) == owner->child_policy)
		{
			owner->saved_observation_generation = owner->observation_generation;
			++owner->saved_observation_revision;
		}
		const bool stream = session->protocol_kind == "claude-code-stream-json" || session->protocol_kind == "antigravity-stream-json";
		if (stream && owner->input_written && owner->native_observed.empty() && !owner->saved_native.empty())
			owner->native_observed = owner->saved_native;
	}

	void AgentRunScheduler::InitialNativeResponse(AcpSessionState& session, const std::string& method, int request_id)
	{
		const auto owner = session.managed_initial_owner;
		const bool native_creation = (session.protocol_kind == "codex-app-server" && method == "thread/start") || (session.protocol_kind == "copilot-sdk" && (method == "session.create" || (owner && owner->resume_only && method == "session.resume"))) || ((session.protocol_kind == "opencode-acp" || session.protocol_kind == "gemini-acp") && method == "session/new");
		if (!owner || owner->session != &session || !native_creation || request_id <= 0 || owner->input_written || owner->input_forbidden || session.session_id.empty() || session.last_process_id.empty() || (!owner->process.empty() && owner->process != session.last_process_id))
			return;
		if (!owner->native_observed.empty() && owner->native_observed != session.session_id)
		{
			ForbidInitialInput(*owner);
			return;
		}
		// The response dispatcher owns method/id correlation; the adapter clears setup id before this receipt callback.
		if (owner->resume_only && session.protocol_kind == "copilot-sdk" && (!owner->expected.managed_continuation || !owner->expected.managed_continuation->execution || session.session_id != owner->expected.managed_continuation->execution->native_session_id))
		{
			ForbidInitialInput(*owner, "The provider changed the saved managed Copilot conversation. No input was replayed.");
			return;
		}
		owner->native_response_seen = true;
		owner->native_observed = session.session_id;
	}

	bool AgentRunScheduler::InitialInputReady(AppState& app, AcpSessionState& session, const ChatSession& chat, bool final_write)
	{
		if (!InitialNeedsOwner(app, chat))
			return true;
		const auto owner = InitialOwner(app, chat);
		if (!owner || session.managed_initial_owner != owner || owner->session != &session)
			return false;
		if (!session.running && owner->process.empty())
			return false;
		if (owner->input_forbidden || !InitialRuntimeOwned(app, *owner) || !owner->completion_acked || session.cancel_requested || session.local_stop_pending || session.remote_stop_pending || !session.running || session.last_process_id != owner->process || session.turn_serial != owner->serial || session.turn_first_user_message_index != owner->row || session.turn_user_message_index != owner->row || owner->input_attempted || owner->row < 0 || static_cast<std::size_t>(owner->row) >= chat.messages.size() || chat.messages[owner->row].created_at != owner->row_created || chat.messages[owner->row].role != MessageRole::User || chat.messages[owner->row].content != InitialTask(*owner) || chat.messages[owner->row].acp_prompt_discarded || !chat.messages[owner->row].attachments.empty() || !chat.messages[owner->row].markdown_store_files.empty() || !chat.messages[owner->row].markdown_store_prompt_blocks.empty())
		{
			ForbidInitialInput(*owner);
			return false;
		}
		const auto deadline = app.agent_run_deadline_steady_ms.find(owner->expected.id);
		if (deadline != app.agent_run_deadline_steady_ms.end() && SteadyMillisecondsNow() >= deadline->second)
		{
			ForbidInitialInput(*owner, "The managed task exceeded its elapsed-time limit before input delivery.");
			return false;
		}
		if (owner->phase != ManagedInitialRuntimeOwner::Phase::Input)
			return false;
		std::string policy_error;
		const auto policy_preflight = ProviderRuntimeRegistry::ResolveById(session.provider_id).OnAcpPrepareInput(app, session, chat, final_write, policy_error);
		if (policy_preflight != AcpRequestPreflight::Ready)
		{
			if (policy_preflight == AcpRequestPreflight::Rejected)
			{
				if (const auto scope = ManagedReadOnlyNativeScope(app, session, chat)) RejectManagedReadPolicy(app, session, chat, *scope, policy_error);
				else ForbidInitialInput(*owner, policy_error.empty() ? "The managed native input policy changed before delivery." : policy_error);
				session.last_error = owner->diagnostic.empty() ? policy_error : owner->diagnostic;
			}
			return false;
		}
		const bool stream = session.protocol_kind == "claude-code-stream-json" || session.protocol_kind == "antigravity-stream-json";
		if (!stream && (!owner->native_response_seen || owner->native_observed.empty() || owner->saved_native != owner->native_observed || session.session_id != owner->native_observed || chat.native_session_id != owner->native_observed || session.pending_local_session_receipt))
			return false;
		if (!ReadPolicyResumeEvidenceMatches(*owner)) return false; // Rechecked before preparation and again at final delivery.
		if (session.startup_model_request_id || session.reasoning_change_request_id || session.config_option_change_request_id || session.mode_change_request_id || session.model_change_request_id || session.awaiting_model_config_options)
			return false;
		const std::string policy = acp_detail::PromptDeliveryConfiguration(app, session, chat);
		if (owner->wire_policy.empty())
			owner->wire_policy = policy;
		if (owner->wire_policy != policy || (final_write && owner->transition))
		{
			ForbidInitialInput(*owner);
			return false;
		}
		if (owner->resume_only && (!owner->expected.managed_continuation->execution || chat.native_session_id != owner->expected.managed_continuation->execution->native_session_id || session.session_id != chat.native_session_id))
		{
			ForbidInitialInput(*owner, "The managed native resume identity changed before input.");
			return false;
		}
		if (final_write && !owner->configuration_captured && !owner->resume_only)
		{
			owner->configuration_captured = true;
			// Failure to capture bounded proof never changes permission for this truthful initial input.
			try
			{
				const auto* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
				const auto* provider = ProviderResolutionService().ProviderForChat(app, chat);
				const bool claude = provider && provider->id == "claude-cli" && session.protocol_kind == "claude-code-stream-json";
				const bool codex = provider && provider->id == "codex-cli" && session.protocol_kind == "codex-app-server" && owner->setup_execution_observed && !owner->binding_withheld && owner->observed_native == session.session_id && owner->observed_native == chat.native_session_id && owner->observed_model == chat.model_id && owner->observed_workspace == chat.workspace_directory && owner->expected.effective_workspace_access == "read" && chat.approval_mode == approval_modes::kPlanApprovalMode;
				const bool copilot = ManagedReadOnlyNativeScope(app, session, chat).has_value();
				const bool gemini = provider && provider->id == "gemini-cli" && session.protocol_kind == "gemini-acp" && owner->setup_execution_observed && !owner->binding_withheld && session.agent_name == "gemini-cli" && !owner->observed_provider_version.empty() && session.agent_version == owner->observed_provider_version && owner->observed_native == session.session_id && owner->observed_native == chat.native_session_id && owner->observed_model == chat.model_id && owner->observed_model == session.current_model_id && owner->observed_workspace == chat.workspace_directory && owner->observed_mode == approval_modes::kPlanApprovalMode && session.current_mode_id == owner->observed_mode && owner->expected.effective_workspace_access == "read" && chat.approval_mode == approval_modes::kPlanApprovalMode;
				if (host && provider && host->id == "local" && host->transport == "local" && chat.routing_mode == "explicit" && (claude || codex || copilot || gemini) && !chat.model_id.empty() && chat.model_id == owner->expected.model_id && owner->expected.depth == 1 && owner->expected.parent_run_id.empty() && owner->expected.managed_continuation->turns.size() == 1 && !owner->expected.managed_continuation->owner.delegation_grant && owner->expected.managed_continuation->turns.front().principal.kind == ManagedRunPrincipalKind::UserRoot)
				{
					ManagedRunExecutionBinding value;
					value.connection_configuration_json = ExecutionRoutingConnectionConfiguration(*host);
					value.provider_configuration_json = ExecutionRoutingProviderConfiguration(app, *provider, host->id);
					value.execution_host_id = chat.execution_host_id;
					value.workspace_directory = chat.workspace_directory;
					value.workspace_worktree_directory = chat.workspace_worktree_directory;
					value.workspace_source_directory = chat.workspace_source_directory;
					value.routing_mode = chat.routing_mode;
					value.routing_source_folder_id = chat.routing_source_folder_id;
					value.routing_source_directory = chat.routing_source_directory;
					value.effective_model_id = chat.model_id;
					value.model_identity_source = "exact-requested";
					value.reasoning_effort = chat.reasoning_effort;
					value.service_tier = chat.service_tier;
					value.service_tier_explicit = chat.service_tier_explicit;
					value.approval_mode = chat.approval_mode;
					value.command_safety_tier = chat.command_safety_tier;
					if (value.connection_configuration_json.size() <= 64U * 1024U && value.provider_configuration_json.size() <= 64U * 1024U)
					{
						owner->dispatch_settings = InitialConfiguration(app, chat);
						owner->dispatch_configuration = std::move(value);
						if (codex) owner->observation_generation = 1; // Gemini still requires its completed prompt's native model telemetry.
					}
				}
			}
			catch (...)
			{
				owner->binding_withheld = true;
			}
		}
		return true;
	}

	void AgentRunScheduler::InitialInputAttempted(AcpSessionState& session)
	{
		if (const auto owner = session.managed_initial_owner; owner && owner->session == &session && owner->serial == session.turn_serial && owner->process == session.last_process_id)
			owner->input_attempted = true;
	}

	void AgentRunScheduler::InitialInputWritten(AcpSessionState& session)
	{
		if (auto owner = session.managed_initial_owner; owner && owner->session == &session && owner->serial == session.turn_serial && owner->process == session.last_process_id)
			owner->input_written = true;
	}

	bool AgentRunScheduler::InitialStopCompletionOwned(const AppState& app, const std::shared_ptr<ManagedInitialRuntimeOwner>& owner)
	{
		if (!owner)
			return true; // Ordinary/v1 completion is unchanged.
		const auto* root = ChatDomainService().FindChatById(app, owner->root_id);
		const auto* child = ChatDomainService().FindChatById(app, owner->transcript_id);
		const auto* session = FindAcpSessionForChat(app, owner->transcript_id);
		return app.data_root == owner->root && root && child && session == owner->session && session && session->managed_initial_owner == owner && root->metadata_incarnation == owner->root_incarnation && child->metadata_incarnation == owner->transcript_incarnation && root->managed_followup_incarnation == owner->root_birth && child->managed_followup_incarnation == owner->transcript_birth && root->created_at == owner->root_created && child->created_at == owner->transcript_created && child->agent_run_id == owner->expected.id;
	}

	void AgentRunScheduler::InitialProcessStopQueued(AcpSessionState& session, AsyncAcpProcessStopTask& task)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->session != &session || owner->process.empty() || session.last_process_id != owner->process)
			return;
		task.managed_initial_owner = owner;
		owner->stop_result = task.result;
		owner->stop_finished = task.finished;
	}

	void AgentRunScheduler::InitialStopRequested(AcpSessionState& session)
	{
		if (const auto owner = session.managed_initial_owner; owner && owner->session == &session)
			ForbidInitialInput(*owner, "The managed initial provider was stopped. Task input was not replayed.");
	}

	void AgentRunScheduler::InitialTurnSettled(AcpSessionState& session, std::string_view outcome, std::string_view error)
	{
		const auto owner = session.managed_initial_owner;
		if (!owner || owner->session != &session || owner->settled || owner->process.empty() || session.turn_serial != owner->serial || (!owner->process.empty() && owner->process != session.last_process_id))
			return;
		owner->settled = true;
		if (owner->desired_status.empty())
		{
			owner->desired_status = outcome == "completed" && owner->input_written ? "completed" : "failed";
			owner->diagnostic_code = owner->desired_status == "completed" ? "" : "provider_turn_failed";
			owner->diagnostic = std::string(error.substr(0, 4096));
		}
		owner->input_forbidden = true;
	}

	namespace
	{
		constexpr std::size_t kInitialBindingPrimaryLimit = 4U * 1024U * 1024U;
		std::string InitialPrimaryDigest(const std::filesystem::path& root, std::string_view id)
		{
			const auto path = AppPaths::UamChatFilePath(root, id);
			std::error_code error;
			const auto status = std::filesystem::symlink_status(path, error);
			if (error || !std::filesystem::is_regular_file(status))
				return {};
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > kInitialBindingPrimaryLimit)
				return {};
			const auto modified = std::filesystem::last_write_time(path, error);
			if (error)
				return {};
			std::ifstream input;
			if (!io::OpenReadFile(path, input))
				return {};
			platform::IncrementalSha256 digest;
			std::array<char, 64U * 1024U> buffer{};
			std::size_t count = 0;
			std::string canonical;
			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto bytes = static_cast<std::size_t>(input.gcount());
				if (bytes > kInitialBindingPrimaryLimit - count || !digest.Add(std::string_view(buffer.data(), bytes)))
					return {};
				count += bytes;
				canonical.append(buffer.data(), bytes);
			}
			if (!input.eof() || count != size)
				return {};
			const auto after = std::filesystem::symlink_status(path, error);
			if (error || !std::filesystem::is_regular_file(after) || std::filesystem::file_size(path, error) != size || error || std::filesystem::last_write_time(path, error) != modified || error)
				return {};
			// This opt-in proof also rejects ambiguous canonical identity keys; it does not alter legacy loading.
			bool unique = true;
			std::unordered_set<std::string> keys;
			const auto document = nlohmann::json::parse(
			    canonical,
			    [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json& value)
			    {
				    if (depth == 1 && event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
					    unique = false;
				    return true;
			    },
			    false);
			if (!unique || !document.is_object())
				return {};
			return digest.Finish();
		}
		bool InitialBindingRootMatches(const std::filesystem::path& root, const AgentRun& run, const std::string& created, std::string& revision_out)
		{
			const auto revision = ChatRepository::ChatStorageRevision(root, run.root_chat_id);
			if (!revision || *revision == "missing")
				return false;
			const auto before = InitialPrimaryDigest(root, run.root_chat_id);
			if (before.empty())
				return false;
			const auto saved = ChatRepository::LoadLocalChatBounded(root, run.root_chat_id, kInitialBindingPrimaryLimit);
			if (!saved || saved->id != run.root_chat_id || !saved->agent_run_id.empty() || saved->created_at != created || saved->managed_followup_incarnation != run.managed_continuation->owner.root_birth_id || saved->imported_read_only || saved->side_cleanup_requested || InitialPrimaryDigest(root, run.root_chat_id) != before || ChatRepository::ChatStorageRevision(root, run.root_chat_id) != revision)
				return false;
			revision_out = *revision;
			return true;
		}
		bool InitialBindingEligible(const AppState& app, const ManagedInitialRuntimeOwner& owner)
		{
			if (owner.expected.provider_id == "gemini-cli")
			{
				const auto* session = FindAcpSessionForChat(app, owner.transcript_id);
				if (!session || session != owner.session || session->managed_initial_owner.get() != &owner || session->provider_id != "gemini-cli" || session->protocol_kind != "gemini-acp" || session->agent_name != "gemini-cli" || owner.observed_provider_version.empty() || session->agent_version != owner.observed_provider_version || session->process_execution_host_id != "local" || session->last_process_id != owner.process || session->session_id != owner.observed_native || session->current_model_id != owner.observed_model || session->current_mode_id != owner.observed_mode) return false;
			}
			const auto* chat = ChatDomainService().FindChatById(app, owner.transcript_id);
			return chat && chat->messages_loaded && !owner.binding_withheld && owner.dispatch_configuration && owner.observation_generation && owner.saved_observation_generation == owner.observation_generation && owner.input_written && owner.input_attempted && owner.settled && owner.desired_status == "completed" && owner.stop_finished && owner.stop_finished->load(std::memory_order_acquire) && owner.stop_result && owner.stop_result->exit_confirmed && InitialStopped(app, owner) && InitialRuntimeOwned(app, owner, false) && chat && InitialChildPolicy(*chat) == owner.child_policy && InitialConfiguration(app, *chat) == owner.dispatch_settings && chat->native_session_id == owner.observed_native && owner.saved_native == owner.observed_native && owner.saved_serial == owner.serial && owner.saved_row == owner.row && owner.saved_row_created == owner.row_created && !app.pending_chat_save_at_by_chat_id.contains(chat->id) && !acp_detail::HasActiveChatSaveWorker(app, chat->id) && !app.worktree_operation_chat_ids.contains(chat->id) && !app.worktree_launch_preparations.contains(chat->id) && !app.execution_routing_restored_unresolved_chat_ids.contains(chat->id);
		}
		bool InitialBindingPublicationOwned(const AppState& app, const ManagedInitialRuntimeOwner& owner, const InitialTransition& work)
		{
			const auto* chat = ChatDomainService().FindChatById(app, owner.transcript_id);
			// Full byte/typed proof is detached. Final permission uses existing cooperative writer revisions,
			// not a claim of detecting an external same-size/restored-mtime rewrite.
			return InitialBindingEligible(app, owner) && owner.observation_generation == work.binding_generation && chat && owner.saved_observation_revision == work.binding_save_revision && chat->messages == work.binding_transcript.messages && !work.binding_primary_revision.empty() && !work.binding_root_revision.empty() && ChatRepository::ChatStorageRevision(owner.root, owner.transcript_id) == std::optional<std::string>(work.binding_primary_revision) && ChatRepository::ChatStorageRevision(owner.root, owner.root_id) == std::optional<std::string>(work.binding_root_revision);
		}

		bool FollowupWitnessMatches(const AppState& app, const ManagedFollowupCompletedOwnerWitness& proof, const AgentRun& run, bool configuration = true)
		{
			const auto* root = ChatDomainService().FindChatById(app, run.root_chat_id);
			if (!FollowupShape(run) || proof.primary_revision.empty() || proof.primary_digest.empty() || !root || proof.root != app.data_root || proof.root_id != run.root_chat_id || proof.run_id != run.id || proof.transcript_id != run.transcript_chat_id || proof.root_incarnation != root->metadata_incarnation || proof.root_created != root->created_at || proof.run_created != run.created_at || proof.provider_id != run.provider_id || proof.agent_id != run.agent_id || proof.definition_hash != run.definition_hash || proof.workspace_access != run.effective_workspace_access || !run.managed_continuation || run.managed_continuation->owner != proof.owner || !run.managed_continuation->execution || *run.managed_continuation->execution != proof.execution || run.managed_continuation->current_turn_id != proof.turn_id || run.managed_continuation->state_revision != proof.state_revision || run.status != "completed" || !run.managed_continuation->turns.back().result_delivered || root->managed_followup_incarnation != proof.owner.root_birth_id || root->side_cleanup_requested || root->imported_read_only) return false;
			return !configuration || (InitialChildPolicy(proof.metadata) == proof.child_policy && InitialConfiguration(app, proof.metadata) == proof.dispatch_settings && FollowupConfigurationMatches(app, proof.metadata, run));
		}
		std::size_t FollowupWitnessBytes(const ManagedFollowupCompletedOwnerWitness& proof)
		{
			// Account retained string capacities/copies, object/vector storage and conservative node/control overhead.
			std::size_t bytes = sizeof(proof) + 512 + (proof.root.native().capacity() + 1) * sizeof(std::filesystem::path::value_type);
			for (const auto* text : {&proof.root_id, &proof.run_id, &proof.transcript_id, &proof.root_created, &proof.run_created, &proof.provider_id, &proof.agent_id, &proof.definition_hash, &proof.workspace_access, &proof.turn_id, &proof.dispatch_settings, &proof.child_policy, &proof.primary_revision, &proof.primary_digest, &proof.owner.root_birth_id, &proof.owner.transcript_birth_id, &proof.execution.connection_configuration_json, &proof.execution.provider_configuration_json, &proof.execution.execution_host_id, &proof.execution.workspace_directory, &proof.execution.workspace_worktree_directory, &proof.execution.workspace_source_directory, &proof.execution.routing_mode, &proof.execution.routing_source_folder_id, &proof.execution.routing_source_directory, &proof.execution.effective_model_id, &proof.execution.model_identity_source, &proof.execution.reasoning_effort, &proof.execution.service_tier, &proof.execution.approval_mode, &proof.execution.command_safety_tier, &proof.execution.native_session_id, &proof.metadata.id, &proof.metadata.agent_run_id, &proof.metadata.managed_followup_incarnation, &proof.metadata.created_at, &proof.metadata.provider_id, &proof.metadata.folder_id, &proof.metadata.execution_host_id, &proof.metadata.workspace_directory, &proof.metadata.workspace_source_directory, &proof.metadata.workspace_worktree_directory, &proof.metadata.workspace_isolation_kind, &proof.metadata.routing_mode, &proof.metadata.routing_source_folder_id, &proof.metadata.routing_source_directory, &proof.metadata.routing_assigned_connection_identity, &proof.metadata.routing_assigned_provider_configuration_identity, &proof.metadata.model_id, &proof.metadata.reasoning_effort, &proof.metadata.service_tier, &proof.metadata.approval_mode, &proof.metadata.command_safety_tier, &proof.metadata.uam_agent_id, &proof.metadata.active_goal_id, &proof.metadata.goal_owner_chat_id, &proof.metadata.temporary_parent_chat_id, &proof.metadata.extra_flags, &proof.metadata.native_session_id, &proof.metadata.memory_level}) bytes += text->capacity() + 1;
			bytes += proof.metadata.routing_inherited_default_fields.capacity() * sizeof(std::string);
			for (const auto& text : proof.metadata.routing_inherited_default_fields) bytes += text.capacity() + 1;
			return bytes;
		}
		void BoundFollowupWitnesses(AppState& app)
		{
			constexpr std::size_t max_count = 128, max_bytes = 16U * 1024U * 1024U;
			const auto within_budget = [&]
			{
				std::unordered_set<const ManagedFollowupCompletedOwnerWitness*> seen;
				std::size_t bytes = 0;
				const auto account = [&](const auto& proof) { if (proof && seen.insert(proof.get()).second) bytes += proof->retained_bytes; };
				for (const auto& [id, proof] : app.managed_followup_completed_owners) account(proof);
				for (const auto& [id, task] : app.managed_followups) if (task) account(task->source);
				return seen.size() <= max_count && bytes <= max_bytes;
			};
			std::unordered_set<const ManagedFollowupCompletedOwnerWitness*> pinned;
			for (const auto& [id, task] : app.managed_followups) if (task && task->source) pinned.insert(task->source.get());
			while (!within_budget() && !app.managed_followup_completed_owners.empty())
			{
				auto oldest = app.managed_followup_completed_owners.end();
				for (auto entry = app.managed_followup_completed_owners.begin(); entry != app.managed_followup_completed_owners.end(); ++entry)
				{
					if (pinned.contains(entry->second.get())) continue; // Real preparation/uncertain bookkeeping still needs exact map ownership.
					if (oldest == app.managed_followup_completed_owners.end() || std::tie(entry->second->retained_at, entry->first) < std::tie(oldest->second->retained_at, oldest->first)) oldest = entry;
				}
				if (oldest == app.managed_followup_completed_owners.end()) break;
				app.managed_followup_completed_owners.erase(oldest); // Withhold only unpinned fresh admission; never rewrite saved data or replay receipts.
			}
		}
		void RetainFollowupWitness(AppState& app, const ManagedInitialRuntimeOwner& owner)
		{
			const auto* child = ChatDomainService().FindChatById(app, owner.transcript_id);
			if (app.managed_initial_shutdown || app.agent_runs.size() > ManagedSubagentControlService::MaxLedgerRuns || !child || !FollowupShape(owner.expected) || !owner.terminal_committed || !owner.input_written || !owner.input_attempted || !owner.settled || owner.desired_status != "completed" || !InitialResultDelivered(owner) || !owner.stop_finished || !owner.stop_finished->load(std::memory_order_acquire) || !owner.stop_result || !owner.stop_result->exit_confirmed || !InitialStopped(app, owner) || !InitialRuntimeOwned(app, owner, false) || (owner.resume_only && !owner.native_response_seen) || !ReadPolicyResumeEvidenceMatches(owner) || owner.completed_primary_digest.empty() || owner.completed_primary_revision.empty() || ChatRepository::ChatStorageRevision(owner.root, owner.transcript_id) != std::optional<std::string>(owner.completed_primary_revision) || owner.dispatch_settings.empty() || owner.child_policy.empty() || InitialConfiguration(app, *child) != owner.dispatch_settings || InitialChildPolicy(*child) != owner.child_policy || owner.dispatch_settings.size() + owner.child_policy.size() + owner.expected.managed_continuation->execution->connection_configuration_json.size() + owner.expected.managed_continuation->execution->provider_configuration_json.size() > 512U * 1024U) return;
			auto proof = std::make_shared<ManagedFollowupCompletedOwnerWitness>();
			proof->root = owner.root; proof->root_id = owner.root_id; proof->run_id = owner.expected.id; proof->transcript_id = owner.transcript_id;
			proof->root_incarnation = owner.root_incarnation; proof->root_created = owner.root_created; proof->run_created = owner.expected.created_at;
			proof->provider_id = owner.expected.provider_id; proof->agent_id = owner.expected.agent_id; proof->definition_hash = owner.expected.definition_hash; proof->workspace_access = owner.expected.effective_workspace_access;
			proof->owner = owner.expected.managed_continuation->owner; proof->execution = *owner.expected.managed_continuation->execution;
			proof->turn_id = owner.expected.managed_continuation->current_turn_id; proof->state_revision = owner.expected.managed_continuation->state_revision;
			proof->dispatch_settings = owner.dispatch_settings; proof->child_policy = owner.child_policy;
			proof->primary_revision = owner.completed_primary_revision; proof->primary_digest = owner.completed_primary_digest;
			// Exact policy metadata only. Never clone messages, queued input, process/session or readiness state.
			auto& metadata = proof->metadata;
			metadata.id = child->id; metadata.agent_run_id = child->agent_run_id; metadata.managed_followup_incarnation = child->managed_followup_incarnation; metadata.created_at = child->created_at;
			metadata.provider_id = child->provider_id; metadata.folder_id = child->folder_id; metadata.execution_host_id = child->execution_host_id;
			metadata.workspace_directory = child->workspace_directory; metadata.workspace_source_directory = child->workspace_source_directory; metadata.workspace_worktree_directory = child->workspace_worktree_directory; metadata.workspace_isolation_kind = child->workspace_isolation_kind;
			metadata.routing_mode = child->routing_mode; metadata.routing_source_folder_id = child->routing_source_folder_id; metadata.routing_source_directory = child->routing_source_directory;
			metadata.routing_assigned_connection_identity = child->routing_assigned_connection_identity; metadata.routing_assigned_provider_configuration_identity = child->routing_assigned_provider_configuration_identity; metadata.routing_inherited_default_fields = child->routing_inherited_default_fields;
			metadata.model_id = child->model_id; metadata.reasoning_effort = child->reasoning_effort; metadata.service_tier = child->service_tier; metadata.service_tier_explicit = child->service_tier_explicit;
			metadata.approval_mode = child->approval_mode; metadata.command_safety_tier = child->command_safety_tier; metadata.uam_agent_id = child->uam_agent_id; metadata.uam_control_enabled = child->uam_control_enabled; metadata.uam_control_explicitly_disabled = child->uam_control_explicitly_disabled;
			metadata.active_goal_id = child->active_goal_id; metadata.goal_owner_chat_id = child->goal_owner_chat_id; metadata.temporary_parent_chat_id = child->temporary_parent_chat_id;
			metadata.imported_read_only = child->imported_read_only; metadata.side_cleanup_requested = child->side_cleanup_requested; metadata.extra_flags = child->extra_flags;
			metadata.native_session_id = child->native_session_id; metadata.memory_enabled = child->memory_enabled; metadata.memory_level = child->memory_level; metadata.small_model_mode = child->small_model_mode;
			metadata.messages_loaded = false;
			if (FollowupWitnessMatches(app, *proof, owner.expected))
			{
				proof->retained_bytes = FollowupWitnessBytes(*proof);
				app.managed_followup_completed_owners[owner.expected.id] = std::move(proof);
				BoundFollowupWitnesses(app);
			}
		}
		void PruneFollowupWitnesses(AppState& app)
		{
			if (app.managed_initial_shutdown || app.agent_runs.size() > ManagedSubagentControlService::MaxLedgerRuns) { app.managed_followup_completed_owners.clear(); return; }
			std::erase_if(app.managed_followup_completed_owners, [&](const auto& entry)
			{
				const auto* run = FindRun(app, entry.first);
				return !entry.second || !run || !FollowupWitnessMatches(app, *entry.second, *run);
			});
		}
		bool FollowupTranscriptIdle(const AppState& app, std::string_view id)
		{
			if (app.worktree_operation_chat_ids.contains(std::string(id)) || app.worktree_launch_preparations.contains(std::string(id)) || app.execution_routing_restored_unresolved_chat_ids.contains(std::string(id)) || app.pending_chat_save_tasks.contains(std::string(id)) || app.pending_chat_save_at_by_chat_id.contains(std::string(id))) return false;
			const auto branch = app.branch_preparations.find(std::string(id));
			if (branch != app.branch_preparations.end() && !branch->second.expired()) return false;
			if (const auto* chat = ChatDomainService().FindChatById(app, std::string(id)); chat && (ChatHasBusyCliTerminal(app, chat->id) || chat->imported_read_only || chat->side_cleanup_requested || chat->managed_followup_input_unavailable || chat->acp_queue_storage_error || !chat->acp_queued_prompts.empty() || chat->remote_turn_reconnect_pending || chat->remote_stop_cleanup_pending || chat->remote_restart_pending || !chat->remote_pending_requests.empty())) return false;
			return std::ranges::none_of(app.acp_sessions, [&](const auto& session) { return session && session->chat_id == id; }); // A retired original lease, not a fake idle session.
		}
		const ChatSession* FollowupRetiredMetadata(const AppState& app, const AgentRun& run)
		{
			if (app.hidden_agent_chat_ownership.data_root != app.data_root) return nullptr;
			const auto& chats = app.hidden_agent_chat_ownership.chats;
			if (std::ranges::count(chats, run.transcript_chat_id, &ChatSession::id) != 1) return nullptr;
			const auto found = std::ranges::find(chats, run.transcript_chat_id, &ChatSession::id);
			const auto& binding = *run.managed_continuation->execution;
			return found->agent_run_id == run.id && found->managed_followup_incarnation == run.managed_continuation->owner.transcript_birth_id && found->native_session_id == binding.native_session_id && found->provider_id == run.provider_id && found->model_id == binding.effective_model_id && found->execution_host_id == binding.execution_host_id && found->workspace_directory == binding.workspace_directory && found->workspace_worktree_directory == binding.workspace_worktree_directory && !found->created_at.empty() ? &*found : nullptr;
		}
		ManagedFollowupCode FollowupScopeCode(const AppState& app, const std::string& root_id, const std::string& run_id)
		{
			if (app.data_root.empty() || app.agent_runs.size() > ManagedSubagentControlService::MaxLedgerRuns || app.chats.size() > ManagedSubagentControlService::MaxChatMetadata || app.hidden_agent_chat_ownership.chats.size() > ManagedSubagentControlService::MaxChatMetadata || std::ranges::count(app.chats, root_id, &ChatSession::id) != 1 || std::ranges::count(app.agent_runs, run_id, &AgentRun::id) != 1) return ManagedFollowupCode::ScopeChanged;
			const auto* root = ChatDomainService().FindChatById(app, root_id);
			const auto* run = FindRun(app, run_id);
			if (!root || !run || run->root_chat_id != root_id || !root->metadata_incarnation || !root->agent_run_id.empty() || !root->temporary_parent_chat_id.empty() || !root->goal_owner_chat_id.empty() || root->imported_read_only || root->side_cleanup_requested || run->transcript_chat_id == root_id || std::ranges::count(app.agent_runs, run->transcript_chat_id, &AgentRun::transcript_chat_id) != 1) return ManagedFollowupCode::ScopeChanged;
			if (!run->managed_continuation) return ManagedFollowupCode::LegacyIdentityUnverified;
			if (root->managed_followup_incarnation != run->managed_continuation->owner.root_birth_id || !uuid::IsCanonicalUuid(root->managed_followup_incarnation) || run->transcript_chat_id.empty()) return ManagedFollowupCode::ScopeChanged;
			// Available original live tokens constrain scope, but their absence never denies canonical replay.
			const auto same_root = [&](const auto& original) { return original.root == app.data_root && original.root_id == root_id && original.root_incarnation == root->metadata_incarnation && original.root_created == root->created_at; };
			if (const auto proof = app.managed_followup_completed_owners.find(run_id); proof != app.managed_followup_completed_owners.end() && proof->second && !same_root(*proof->second)) return ManagedFollowupCode::ScopeChanged;
			if (const auto owner = app.managed_initial_runtime_owners.find(run_id); owner != app.managed_initial_runtime_owners.end() && owner->second && !same_root(*owner->second)) return ManagedFollowupCode::ScopeChanged;
			if (const auto task = app.managed_followups.find(run_id); task != app.managed_followups.end() && task->second && (task->second->root != app.data_root || task->second->root_id != root_id || task->second->incarnation != root->metadata_incarnation || task->second->root_created != root->created_at)) return ManagedFollowupCode::ScopeChanged;
			if (run->parent_run_id.empty())
			{
				if (run->depth != 1) return ManagedFollowupCode::ScopeChanged;
			}
			else
			{
				const auto* parent = FindRun(app, run->parent_run_id);
				if (run->depth != 2 || std::ranges::count(app.agent_runs, run->parent_run_id, &AgentRun::id) != 1 || !parent || parent->root_chat_id != root_id || parent->depth != 1 || !parent->parent_run_id.empty()) return ManagedFollowupCode::ScopeChanged;
			}
			return ManagedFollowupCode::None;
		}
		ManagedFollowupCode FollowupLiveCode(const AppState& app, const std::string& root_id, const std::string& run_id, bool replay = false)
		{
			const auto scope = FollowupScopeCode(app, root_id, run_id);
			if (scope != ManagedFollowupCode::None) return scope;
			const auto* root = ChatDomainService().FindChatById(app, root_id);
			const auto* run = FindRun(app, run_id);
			if (!run->managed_continuation->execution || run->managed_continuation->execution->model_identity_source.empty()) return ManagedFollowupCode::ModelIdentityUnverified;
			if (!FollowupShape(*run)) return ManagedFollowupCode::NativeResumeUnavailable;
			if (app.managed_initial_shutdown || app.deletion_progress || app.managed_initial_mentions.contains(root_id) || app.managed_followups.contains(run_id)) return ManagedFollowupCode::Busy;
			if (replay) return ManagedFollowupCode::None; // Canonical immutable receipt replay is not a new execution admission.
			const auto proof = app.managed_followup_completed_owners.find(run_id);
			if (proof == app.managed_followup_completed_owners.end() || !proof->second || !FollowupWitnessMatches(app, *proof->second, *run)) return ManagedFollowupCode::NativeResumeUnavailable;
			if (!replay && (app.pending_agent_run_cancellation_ids.contains(run_id) || app.managed_subagent_cancellation_receipts.contains(run_id))) return ManagedFollowupCode::CancelPending;
			if (!replay && (app.managed_initial_runtime_owners.contains(run_id) || !FollowupRetiredMetadata(app, *run) || AgentRunScheduler::HasPendingInitialMention(app, root_id) || !InitialRootIdle(app, *root, false) || !FollowupTranscriptIdle(app, run->transcript_chat_id))) return ManagedFollowupCode::Busy;
			const auto& binding = *run->managed_continuation->execution;
			const auto* host = execution_hosts::Find(app.settings.execution_hosts, binding.execution_host_id);
			const auto* retired = FollowupRetiredMetadata(app, *run);
			if (!retired) return ManagedFollowupCode::Busy;
			const auto* provider = ProviderResolutionService().ProviderForChat(app, *retired);
			if (!host || host->transport != "local" || !provider || provider->id != run->provider_id || !provider->supports_resume || !provider->supports_structured || !FollowupProtocolMatches(provider->id, provider->structured_protocol) || !provider_availability::IsEnabled(app.settings, provider->id, host->id) || ExecutionRoutingConnectionConfiguration(*host) != binding.connection_configuration_json || ExecutionRoutingProviderConfiguration(app, *provider, host->id) != binding.provider_configuration_json) return ManagedFollowupCode::PermissionDenied;
			if (!replay)
			{
				const auto& turn = run->managed_continuation->turns.back();
				if (run->status != "completed" || (turn.result_required && !turn.result_delivered)) return ManagedFollowupCode::Busy;
				if (run->managed_continuation->turns.size() >= 33) return ManagedFollowupCode::ReceiptLimit;
				if (std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status) + app.managed_initial_mentions.size() + app.managed_followups.size() >= kMaxQueuedAgents || std::ranges::count_if(app.agent_runs, [&](const auto& value) { return value.root_chat_id == root_id && value.parent_run_id.empty() && !IsTerminal(value.status); }) >= kMaxDirectChildren) return ManagedFollowupCode::QueueFull;
			}
			return ManagedFollowupCode::None;
		}
		bool FollowupRequestMatches(const ManagedRunTurnReceipt& turn, const ManagedFollowupRequest& request)
		{
			return turn.client_request_id == request.client_request_id && turn.after_turn_id == request.after_turn_id && turn.text == request.text && turn.continuity == request.continuity && turn.recovery_decision == request.recovery_decision && turn.retry_of_turn_id == request.retry_of_turn_id;
		}
		ManagedFollowupAcceptance FollowupAcceptance(const AgentRun& run, const ManagedRunTurnReceipt& turn)
		{
			return {run.root_chat_id, run.id, run.transcript_chat_id, turn.client_request_id, turn.turn_id, turn.turn_number, ManagedRunTurnStatus::Queued};
		}
		bool FollowupCapturedRootMatches(const std::filesystem::path& root, ChatSession saved, const ChatSession& captured)
		{
			if (captured.messages_loaded && saved.messages != captured.messages) return false;
			saved.messages_loaded = captured.messages_loaded;
			saved.persisted_message_count = captured.persisted_message_count;
			saved.persisted_messages_digest = captured.persisted_messages_digest;
			saved.acp_queue_full_save_revision = captured.acp_queue_full_save_revision;
			return ChatRepository::ChatMetadataFingerprint(root, saved) == ChatRepository::ChatMetadataFingerprint(root, captured);
		}
		std::string FollowupLedgerStamp(const std::filesystem::path& root, const std::string& id)
		{
			const auto path = root / "agent-runs" / (id + ".json");
			std::error_code error;
			if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error) return {};
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > 512U * 1024U) return {};
			const auto modified = std::filesystem::last_write_time(path, error);
			if (error) return {};
			const auto ticks = modified.time_since_epoch().count();
			if (ticks < std::numeric_limits<int64_t>::min() || ticks > std::numeric_limits<int64_t>::max()) return {};
			return std::to_string(size) + ":" + std::to_string(static_cast<int64_t>(ticks));
		}
		bool FollowupTaskScope(const AppState& app, const PendingManagedFollowup& task)
		{
			const auto* root = ChatDomainService().FindChatById(app, task.root_id);
			return task.root == app.data_root && root && root->metadata_incarnation == task.incarnation && root->created_at == task.root_created && root->managed_followup_incarnation == task.root_birth && std::ranges::count(app.chats, task.root_id, &ChatSession::id) == 1;
		}
		bool FollowupTaskOwned(const AppState& app, const PendingManagedFollowup& task, bool bookkeeping = false)
		{
			const auto& work = task.work;
			const auto* root = ChatDomainService().FindChatById(app, task.root_id);
			const auto* run = FindRun(app, task.run_id);
			if (!work || !FollowupTaskScope(app, task) || !run || !InitialRunEquals(*run, work->expected) || (!bookkeeping && (task.cancelled || app.managed_initial_shutdown || work->cancel.load() || InitialClock::now() >= task.deadline)) || app.deletion_progress || app.managed_initial_mentions.contains(task.root_id) || ChatRepository::ChatMetadataFingerprint(app.data_root, *root) != task.metadata || (work->parent.messages_loaded && root->messages != work->parent.messages) || InitialConfiguration(app, *root) != task.settings) return false;
			if (task.replay) return true;
			const auto proof = app.managed_followup_completed_owners.find(task.run_id);
			if (!task.source || proof == app.managed_followup_completed_owners.end() || proof->second != task.source || !FollowupWitnessMatches(app, *task.source, work->expected) || work->transcript_revision != task.source->primary_revision || work->predecessor_digest != task.source->primary_digest || work->transcript.created_at != task.source->metadata.created_at || InitialChildPolicy(work->transcript) != task.source->child_policy || InitialConfiguration(app, work->transcript) != task.source->dispatch_settings) return false;
			if (bookkeeping && (ChatRepository::ChatStorageRevision(task.root, task.root_id) != std::optional<std::string>(work->root_revision) || ChatRepository::ChatStorageRevision(task.root, task.transcript_id) != std::optional<std::string>(work->transcript_revision))) return false;
			if (!InitialRootIdle(app, *root, false) || AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, *root) || !FollowupTranscriptIdle(app, task.transcript_id) || app.managed_initial_runtime_owners.contains(task.run_id)) return false;
			if (!bookkeeping && (std::ranges::count(app.agent_runs, std::string("queued"), &AgentRun::status) >= kMaxQueuedAgents || std::ranges::count_if(app.agent_runs, [&](const auto& value) { return value.root_chat_id == task.root_id && value.parent_run_id.empty() && !IsTerminal(value.status); }) >= kMaxDirectChildren)) return false;
			const auto* child = ChatDomainService().FindChatById(app, task.transcript_id);
			if (work->live_transcript)
			{
				if (!child || child->metadata_incarnation != work->live_transcript->metadata_incarnation || child->messages != work->live_transcript->messages || ChatRepository::ChatMetadataFingerprint(app.data_root, *child) != ChatRepository::ChatMetadataFingerprint(task.root, *work->live_transcript)) return false;
			}
			else if (child) return false;
			return std::ranges::count(app.chats, task.transcript_id, &ChatSession::id) <= 1 && FollowupConfigurationMatches(app, work->transcript, work->expected);
		}
		void PrepareFollowup(FollowupPreparation& work, const ManagedFollowupRequest& request, bool replay)
		{
			work.code = ManagedFollowupCode::PersistenceFailed;
			const auto ledger_path = work.root / "agent-runs" / (work.expected.id + ".json");
			const auto stamp = FollowupLedgerStamp(work.root, work.expected.id);
			std::string raw;
			if (stamp.empty() || !io::TryReadTextFile(ledger_path, raw, 512U * 1024U)) { work.error = "The managed primary ledger is unavailable."; return; }
			const auto loaded = AgentRunLedger::LoadAllReadOnly(work.root);
			const auto found = std::ranges::find(loaded.runs, work.expected.id, &AgentRun::id);
			std::string after;
			if (!loaded.errors.empty() || found == loaded.runs.end() || !InitialRunEquals(*found, work.expected) || !io::TryReadTextFile(ledger_path, after, 512U * 1024U) || raw != after || FollowupLedgerStamp(work.root, work.expected.id) != stamp) { work.error = "The managed primary ledger changed."; return; }
			work.ledger_stamp = stamp;
			work.root_revision = ChatRepository::ChatStorageRevision(work.root, work.parent.id).value_or("");
			const auto root_digest = InitialPrimaryDigest(work.root, work.parent.id);
			const auto parent = ChatRepository::LoadLocalChatBounded(work.root, work.parent.id, kInitialBindingPrimaryLimit);
			if (work.cancel.load() || !parent || root_digest.empty() || !FollowupCapturedRootMatches(work.root, *parent, work.parent) || parent->managed_followup_incarnation != work.expected.managed_continuation->owner.root_birth_id || InitialPrimaryDigest(work.root, work.parent.id) != root_digest || ChatRepository::ChatStorageRevision(work.root, work.parent.id) != std::optional<std::string>(work.root_revision)) { work.code = ManagedFollowupCode::ScopeChanged; work.error = "The saved parent no longer matches this request."; return; }
			if (replay)
			{
				if (std::ranges::none_of(found->managed_continuation->turns, [&](const auto& turn) { return FollowupRequestMatches(turn, request); })) { work.code = ManagedFollowupCode::IdempotencyConflict; work.error = "The stored client request differs."; }
				else work.code = ManagedFollowupCode::None;
				return;
			}
			work.transcript_revision = ChatRepository::ChatStorageRevision(work.root, work.expected.transcript_chat_id).value_or("");
			const auto digest = InitialPrimaryDigest(work.root, work.expected.transcript_chat_id);
			auto transcript = ChatRepository::LoadLocalChatBounded(work.root, work.expected.transcript_chat_id, kInitialBindingPrimaryLimit);
			if (work.cancel.load() || !transcript || digest.empty() || digest != work.predecessor_digest || work.transcript_revision != work.predecessor_revision || work.transcript_revision.empty() || work.transcript_revision == "missing" || transcript->id != work.expected.transcript_chat_id || transcript->agent_run_id != work.expected.id || transcript->created_at.empty() || transcript->managed_followup_incarnation != work.expected.managed_continuation->owner.transcript_birth_id || transcript->native_session_id != work.expected.managed_continuation->execution->native_session_id || transcript->imported_read_only || transcript->side_cleanup_requested || transcript->managed_followup_input_unavailable || transcript->acp_queue_storage_error || !transcript->acp_queued_prompts.empty() || !transcript->remote_pending_requests.empty() || transcript->remote_turn_reconnect_pending || transcript->remote_stop_cleanup_pending || transcript->remote_restart_pending || InitialPrimaryDigest(work.root, transcript->id) != digest || ChatRepository::ChatStorageRevision(work.root, transcript->id) != std::optional<std::string>(work.transcript_revision)) { work.code = ManagedFollowupCode::TranscriptUnavailable; work.error = "The exact native managed transcript is unavailable."; return; }
			const auto& previous = found->managed_continuation->turns.back();
			if (previous.first_user_message_index < 0 || static_cast<std::size_t>(previous.first_user_message_index) >= transcript->messages.size() || transcript->messages[previous.first_user_message_index].role != MessageRole::User || transcript->messages[previous.first_user_message_index].content != (found->managed_continuation->turns.size() == 1 ? found->task : previous.text) || transcript->messages[previous.first_user_message_index].created_at != previous.user_message_created_at || transcript->messages[previous.first_user_message_index].acp_prompt_discarded || (work.live_transcript && !InitialStoredChatMatches(work.root, *transcript, *work.live_transcript))) { work.code = ManagedFollowupCode::TranscriptUnavailable; work.error = "The predecessor's saved input row changed."; return; }
			const auto catalog = AgentDefinitionService::Load(work.root, transcript->workspace_directory);
			const auto* definition = FindAgent(catalog, work.expected.agent_id);
			if (!definition || definition->definition_hash != work.expected.definition_hash || definition->markdown_snapshot != work.expected.definition_snapshot || definition->instructions != work.expected.definition_instructions || definition->skills != work.expected.skills_snapshot || definition->delegates != work.expected.delegates_snapshot) { work.code = ManagedFollowupCode::PermissionDenied; work.error = "The saved managed definition is no longer available unchanged."; return; }
			std::size_t deliveries = 0;
			for (const auto& message : parent->messages)
				for (const auto& tool : message.tool_calls)
					if (tool.id == "uam-agent-result-" + previous.delivery_id)
					{
						const auto result = nlohmann::json::parse(tool.result_text, nullptr, false);
						if (!result.is_object() || result.value("runId", std::string{}) != work.expected.id || result.value("status", std::string{}) != "completed" || result.value("result", std::string{}) != work.expected.result_excerpt || (previous.turn_number > 1 && (result.value("turnId", std::string{}) != previous.turn_id || result.value("turnNumber", std::uint32_t{0}) != previous.turn_number))) { work.code = ManagedFollowupCode::TranscriptUnavailable; work.error = "The predecessor result changed."; return; }
						++deliveries;
					}
			if (deliveries != 1 || ResultExcerpt(*transcript, static_cast<std::size_t>(previous.first_user_message_index + 1)) != work.expected.result_excerpt) { work.code = ManagedFollowupCode::TranscriptUnavailable; work.error = "The predecessor result is unavailable."; return; }
			work.transcript = std::move(*transcript);
			work.transcript.metadata_incarnation = work.live_transcript ? work.live_transcript->metadata_incarnation : std::make_shared<const std::uint8_t>(0);
			work.prepared = AgentRunLedger::PrepareSave(work.root, work.expected, work.candidate, &work.error);
			if (work.prepared && work.error.empty()) work.code = ManagedFollowupCode::None;
		}
		void CleanFollowup(PendingManagedFollowup& task)
		{
			task.phase = PendingManagedFollowup::Phase::Cleaning;
			task.work.reset(); // Worker-owned RAII retains consumed uncertain evidence, cleans only unused own stages.
		}
		void InstallFollowupOwner(AppState& app, PendingManagedFollowup& task, bool confirmed_acceptance)
		{
			const auto work = task.work;
			*FindRun(app, task.run_id) = work->candidate;
			if (auto* child = ChatDomainService().FindChatById(app, task.transcript_id)) *child = work->transcript;
			else app.chats.push_back(work->transcript);
			const auto* root = ChatDomainService().FindChatById(app, task.root_id);
			const auto* child = ChatDomainService().FindChatById(app, task.transcript_id);
			auto owner = std::make_shared<ManagedInitialRuntimeOwner>();
			owner->resume_only = true;
			owner->root = task.root; owner->root_id = task.root_id; owner->transcript_id = task.transcript_id;
			owner->root_incarnation = task.incarnation; owner->transcript_incarnation = child->metadata_incarnation;
			owner->root_birth = work->candidate.managed_continuation->owner.root_birth_id; owner->transcript_birth = work->candidate.managed_continuation->owner.transcript_birth_id;
			owner->root_created = root->created_at; owner->transcript_created = child->created_at;
			owner->root_policy = InitialChildPolicy(*root); owner->root_settings = InitialConfiguration(app, *root);
			owner->settings = InitialConfiguration(app, *child); owner->child_policy = InitialChildPolicy(*child);
			owner->expected = work->candidate;
			owner->dispatch_settings = task.source->dispatch_settings;
			owner->configuration_captured = true;
			owner->dispatch_configuration = work->candidate.managed_continuation->execution;
			if (!confirmed_acceptance)
			{
				owner->input_forbidden = true;
				owner->desired_status = app.managed_initial_shutdown ? "interrupted" : task.cancelled ? "cancelled" : "failed";
				owner->diagnostic_code = "followup_delivery_unknown";
				owner->diagnostic = "Managed admission was unconfirmed. No task input was attempted or replayed.";
			}
			app.managed_initial_runtime_owners.emplace(task.run_id, owner);
			app.managed_followup_completed_owners.erase(task.run_id);
			++ChatDomainService().FindChatById(app, task.root_id)->managed_subagent_revision;
		}
		bool PollFollowupCreations(AppState& app)
		{
			bool changed = false;
			std::vector<std::string> ids;
			for (const auto& [id, task] : app.managed_followups) ids.push_back(id);
			for (const auto& id : ids)
			{
				const auto found = app.managed_followups.find(id);
				if (found == app.managed_followups.end()) continue;
				auto task = found->second;
				if (task->phase == PendingManagedFollowup::Phase::Preparing)
				{
					if (task->cancelled || app.managed_initial_shutdown || !FollowupTaskScope(app, *task)) task->work->cancel.store(true);
					if (!task->work->finished.load(std::memory_order_acquire)) continue;
					auto work = task->work;
					const auto owned = [&] { return FollowupTaskOwned(app, *task) && ChatRepository::ChatStorageRevision(task->root, task->root_id) == std::optional<std::string>(work->root_revision) && (task->replay || ChatRepository::ChatStorageRevision(task->root, task->transcript_id) == std::optional<std::string>(work->transcript_revision)); };
					if (work->code != ManagedFollowupCode::None || !work->error.empty() || !owned())
					{
						task->result.code = task->cancelled ? ManagedFollowupCode::CancelPending : work->code != ManagedFollowupCode::None ? work->code : ManagedFollowupCode::ScopeChanged;
						CleanFollowup(*task); continue;
					}
					if (task->replay)
					{
						if (FollowupLedgerStamp(task->root, task->run_id) != work->ledger_stamp) task->result.code = ManagedFollowupCode::Busy;
						else
						{
							const auto receipt = std::ranges::find(work->expected.managed_continuation->turns, task->request.client_request_id, &ManagedRunTurnReceipt::client_request_id);
							task->result = {true, true, ManagedFollowupCode::None, FollowupAcceptance(work->expected, *receipt)};
						}
						CleanFollowup(*task); continue;
					}
					const auto publication = AgentRunLedger::PublishPreparedSave(work->prepared, owned);
					if (publication.state == AgentRunPublishState::Committed)
					{
						InstallFollowupOwner(app, *task, true); // Ledger, same live run/body and owner ACK on this one serialized stack.
						task->accepted = true;
						task->result = {true, false, ManagedFollowupCode::None, FollowupAcceptance(work->candidate, work->candidate.managed_continuation->turns.back())};
						CleanFollowup(*task); changed = true;
					}
					else if (publication.state == AgentRunPublishState::Uncertain)
					{
						task->result.code = ManagedFollowupCode::DeliveryUnknown;
						task->phase = PendingManagedFollowup::Phase::Uncertain;
						task->retry_at = InitialClock::now() + std::chrono::milliseconds(500);
						try { (void)task->completion(task->result); } catch (...) { diagnostics::Write("Managed follow-up completion failed."); }
						task->completion_settled = true; changed = true;
					}
					else { task->result.code = ManagedFollowupCode::PersistenceFailed; CleanFollowup(*task); }
				}
				if (task->phase == PendingManagedFollowup::Phase::Uncertain)
				{
					const auto work = task->work;
					if (app.managed_initial_shutdown || !FollowupTaskScope(app, *task) || InitialClock::now() < task->retry_at) continue;
					if (!task->reconciling)
					{
						task->reconciling = true;
						StartInitialPreparation(work, [](FollowupPreparation& work)
						{
							work.observed.reset();
							const auto loaded = AgentRunLedger::LoadAllReadOnly(work.root);
							if (!loaded.errors.empty()) return;
							for (const auto& run : loaded.runs)
								if (run.id == work.expected.id && (InitialRunEquals(run, work.expected) || InitialRunEquals(run, work.candidate))) work.observed = run;
						});
						continue;
					}
					if (!work->finished.load(std::memory_order_acquire)) continue;
					task->reconciling = false; task->retry_at = InitialClock::now() + std::chrono::milliseconds(500);
					if (!work->observed || !FollowupTaskOwned(app, *task, true)) continue;
					if (InitialRunEquals(*work->observed, work->candidate)) InstallFollowupOwner(app, *task, false);
					CleanFollowup(*task); changed = true; // Bookkeeping only, never a second permission/input attempt.
				}
				if (task->phase != PendingManagedFollowup::Phase::Cleaning || !task->retired->load(std::memory_order_acquire)) continue;
				bool acked = false;
				if (!task->completion_settled)
				{
					try { acked = task->completion(task->result); } catch (...) { diagnostics::Write("Managed follow-up completion failed."); }
					task->completion_settled = true;
				}
				if (task->accepted)
				{
					const auto accepted = app.managed_initial_runtime_owners.find(task->run_id);
					if (accepted != app.managed_initial_runtime_owners.end())
					{
						auto& owner = *accepted->second;
						owner.completion_acked = acked;
						if (acked && !task->cancelled && !app.managed_initial_shutdown && !app.pending_agent_run_cancellation_ids.contains(id) && InitialRuntimeOwned(app, owner)) app.queued_agent_run_ids.push_back(id);
						else ForbidInitialInput(owner, "The recorded follow-up lost its completion owner. No input was sent.");
					}
				}
				app.managed_followups.erase(id); changed = true;
			}
			return changed;
		}
		bool BeginInitialTransition(AppState& app, ManagedInitialRuntimeOwner& owner, AgentRun candidate, ManagedInitialRuntimeOwner::Action action)
		{
			if (owner.transition || (owner.retired && !owner.retired->load(std::memory_order_acquire)) || InitialClock::now() < owner.retry_at)
				return false;
			auto work = native_history::TryCreateWorkerOwnedNativeRefresh<InitialTransition>();
			if (!work)
				return false;
			owner.retired = std::make_shared<std::atomic<bool>>(false);
			work->retired = owner.retired;
			work->expected = owner.expected;
			work->candidate = std::move(candidate);
			owner.transition = work;
			owner.action = action;
			const auto root = owner.root;
			if (owner.resume_only && action == ManagedInitialRuntimeOwner::Action::Terminal)
			{
				work->resume_terminal = true;
				work->binding_transcript = *ChatDomainService().FindChatById(app, owner.transcript_id);
			}
			std::optional<ManagedRunExecutionBinding> binding;
			if (action == ManagedInitialRuntimeOwner::Action::Terminal && work->candidate.status == "completed" && !work->candidate.managed_continuation->execution && InitialBindingEligible(app, owner))
			{
				std::error_code error;
				const auto size = std::filesystem::file_size(AppPaths::UamChatFilePath(root, owner.transcript_id), error);
				if (!error && size <= kInitialBindingPrimaryLimit)
				{
					binding = owner.dispatch_configuration;
					binding->native_session_id = owner.observed_native;
					work->binding_requested = true;
					work->binding_generation = owner.observation_generation;
					work->binding_save_revision = owner.saved_observation_revision;
					work->binding_root_created = owner.root_created;
					work->binding_transcript = *ChatDomainService().FindChatById(app, owner.transcript_id);
				}
			}
			StartInitialPreparation(work,
			                        [root, binding](InitialTransition& work)
			                        {
				                        if (work.resume_terminal)
				                        {
					                        const auto& turn = work.expected.managed_continuation->turns.back();
					                        const auto before = InitialPrimaryDigest(root, work.expected.transcript_chat_id);
					                        const auto revision = ChatRepository::ChatStorageRevision(root, work.expected.transcript_chat_id);
					                        const auto saved = ChatRepository::LoadLocalChatBounded(root, work.expected.transcript_chat_id, kInitialBindingPrimaryLimit);
					                        const bool row = turn.first_user_message_index < 0 ? !turn.dispatch_attempted || turn.expected_turn_serial == 0 : saved && static_cast<std::size_t>(turn.first_user_message_index) < saved->messages.size() && saved->messages[turn.first_user_message_index].role == MessageRole::User && saved->messages[turn.first_user_message_index].content == turn.text && saved->messages[turn.first_user_message_index].created_at == turn.user_message_created_at && !saved->messages[turn.first_user_message_index].acp_prompt_discarded;
					                        if (!saved || !row || before.empty() || !revision || saved->native_session_id != work.expected.managed_continuation->execution->native_session_id || !InitialStoredChatMatches(root, *saved, work.binding_transcript) || InitialPrimaryDigest(root, saved->id) != before || ChatRepository::ChatStorageRevision(root, saved->id) != revision)
					                        {
						                        work.error = "The exact managed current-turn transcript save remains unconfirmed.";
						                        return;
					                        }
					                        work.binding_primary_revision = *revision;
					                        work.binding_primary_digest = before;
				                        }
				                        // Detached canonical proof; no live AppState/session/owner reads on this worker.
				                        if (binding)
				                        {
					                        const auto revision = ChatRepository::ChatStorageRevision(root, work.expected.transcript_chat_id);
					                        const auto before = revision && *revision != "missing" ? InitialPrimaryDigest(root, work.expected.transcript_chat_id) : std::string{};
					                        const auto saved = before.empty() ? std::optional<ChatSession>{} : ChatRepository::LoadLocalChatBounded(root, work.expected.transcript_chat_id, kInitialBindingPrimaryLimit);
					                        const auto& turn = work.expected.managed_continuation->turns.front();
					                        if (saved && saved->native_session_id == binding->native_session_id && saved->model_id == binding->effective_model_id && saved->agent_run_id == work.expected.id && saved->provider_id == work.expected.provider_id && saved->managed_followup_incarnation == work.expected.managed_continuation->owner.transcript_birth_id && turn.dispatch_attempted && turn.expected_turn_serial == work.expected.expected_turn_serial && turn.expected_turn_serial > 0 && turn.first_user_message_index >= 0 && static_cast<std::size_t>(turn.first_user_message_index) < saved->messages.size() && saved->messages[turn.first_user_message_index].role == MessageRole::User && !saved->messages[turn.first_user_message_index].acp_prompt_discarded && saved->messages[turn.first_user_message_index].content == work.expected.task && saved->messages[turn.first_user_message_index].created_at == turn.user_message_created_at && InitialStoredChatMatches(root, *saved, work.binding_transcript) && InitialPrimaryDigest(root, saved->id) == before && ChatRepository::ChatStorageRevision(root, saved->id) == revision && InitialBindingRootMatches(root, work.expected, work.binding_root_created, work.binding_root_revision))
					                        {
						                        work.binding_primary_revision = *revision;
						                        work.binding_primary_digest = before;
						                        work.candidate.managed_continuation->execution = binding;
					                        }
				                        }
				                        work.prepared = AgentRunLedger::PrepareSave(root, work.expected, work.candidate, &work.error);
				                        if (!work.prepared && work.candidate.managed_continuation->execution && binding)
				                        {
					                        // No permission or prepared publication exists: try a new truthful unbound terminal value.
					                        auto absent = work.candidate;
					                        absent.managed_continuation->execution.reset();
					                        std::string error;
					                        auto prepared = AgentRunLedger::PrepareSave(root, work.expected, absent, &error);
					                        work.candidate = std::move(absent);
					                        work.prepared = std::move(prepared);
					                        work.error = std::move(error);
				                        }
			                        });
			return true;
		}
		AgentRun InitialCandidate(const ManagedInitialRuntimeOwner& owner)
		{
			AgentRun candidate = owner.expected;
			++candidate.managed_continuation->state_revision;
			candidate.updated_at = time::TimestampNow();
			return candidate;
		}
		void StartInitialReconciliation(ManagedInitialRuntimeOwner& owner)
		{
			if (owner.transition || !owner.uncertain_candidate || (owner.retired && !owner.retired->load(std::memory_order_acquire)) || InitialClock::now() < owner.retry_at)
				return;
			auto work = native_history::TryCreateWorkerOwnedNativeRefresh<InitialTransition>();
			if (!work)
				return;
			owner.retired = std::make_shared<std::atomic<bool>>(false);
			work->retired = owner.retired;
			work->expected = owner.expected;
			work->candidate = *owner.uncertain_candidate;
			owner.transition = work;
			owner.action = ManagedInitialRuntimeOwner::Action::Reconcile;
			const auto root = owner.root;
			StartInitialPreparation(work,
			                        [root](InitialTransition& work)
			                        {
				                        // Read-only observation can reconcile bookkeeping, never grant another input attempt.
				                        const auto loaded = AgentRunLedger::LoadAllReadOnly(root);
				                        for (const auto& run : loaded.runs)
					                        if (run.id == work.expected.id && (InitialRunEquals(run, work.expected) || InitialRunEquals(run, work.candidate)))
						                        work.observed = run;
				                        if (!work.observed)
					                        work.error = "The managed ledger no longer matches either exact publication outcome.";
			                        });
		}
		bool PollInitialTransition(AppState& app, ManagedInitialRuntimeOwner& owner)
		{
			const auto work = owner.transition;
			if (!work || !work->finished.load(std::memory_order_acquire))
				return false;
			const auto action = owner.action;
			if (action == ManagedInitialRuntimeOwner::Action::Reconcile)
			{
				if (work->observed && InitialRuntimeOwned(app, owner, false))
				{
					*FindRun(app, owner.expected.id) = *work->observed;
					owner.expected = *work->observed;
					owner.uncertain_candidate.reset();
					if (IsTerminal(owner.expected.status))
						owner.phase = ManagedInitialRuntimeOwner::Phase::Result;
				}
				owner.retry_at = InitialClock::now() + std::chrono::milliseconds(500);
				owner.transition.reset();
				return true;
			}
			if (action == ManagedInitialRuntimeOwner::Action::Parent)
				return false;
			const bool input_transition = action == ManagedInitialRuntimeOwner::Action::Running || action == ManagedInitialRuntimeOwner::Action::Row;
			const bool binding_terminal = action == ManagedInitialRuntimeOwner::Action::Terminal && work->binding_requested && work->candidate.managed_continuation->execution.has_value();
			if (work->binding_requested && !work->candidate.managed_continuation->execution)
				owner.binding_withheld = true;
			const auto abandon_binding = [&]
			{
				// Discard only this unused prepared value. The next terminal action prepares a fresh absent value.
				owner.binding_withheld = true;
				owner.transition.reset();
			};
			if (binding_terminal && !InitialBindingEligible(app, owner))
			{
				abandon_binding();
				return true;
			}
			const auto resume_saved = [&]
			{
				if (!work->resume_terminal) return true;
				const auto* chat = ChatDomainService().FindChatById(app, owner.transcript_id);
				return chat && chat->messages == work->binding_transcript.messages && !work->binding_primary_revision.empty() && ChatRepository::ChatStorageRevision(owner.root, owner.transcript_id) == std::optional<std::string>(work->binding_primary_revision);
			};
			if (work->resume_terminal && work->prepared && !resume_saved()) { owner.transition.reset(); return true; }
			const auto base_owned = [&] { return InitialRuntimeOwned(app, owner, input_transition) && (!input_transition || !owner.input_forbidden) && resume_saved(); };
			const auto owned = [&] { return base_owned() && (!binding_terminal || InitialBindingPublicationOwned(app, owner, *work)); };
			AgentRunPublishResult result;
			if (work->prepared && work->error.empty() && base_owned())
				result = AgentRunLedger::PublishPreparedSave(work->prepared, owned);
			if (result.state == AgentRunPublishState::Rejected && binding_terminal && !InitialBindingPublicationOwned(app, owner, *work))
			{
				abandon_binding();
				return true;
			}
			if (result.state != AgentRunPublishState::Committed)
			{
				// Even a rejected first-attempt CAS permanently removes input permission.
				ForbidInitialInput(owner, "Managed state could not be confirmed. UAM retained the record and did not replay task input.");
				owner.uncertain_candidate = work->candidate;
				owner.retry_at = InitialClock::now() + std::chrono::milliseconds(500);
				owner.transition.reset();
				return true;
			}
			*FindRun(app, owner.expected.id) = work->candidate;
			owner.expected = work->candidate;
			if (auto* root = ChatDomainService().FindChatById(app, owner.root_id))
				++root->managed_subagent_revision;
			if (action == ManagedInitialRuntimeOwner::Action::Running)
				owner.phase = ManagedInitialRuntimeOwner::Phase::Starting;
			if (action == ManagedInitialRuntimeOwner::Action::Row)
				owner.phase = ManagedInitialRuntimeOwner::Phase::Input;
			if (action == ManagedInitialRuntimeOwner::Action::Terminal)
			{
				owner.terminal_committed = true;
				owner.completed_primary_revision = work->binding_primary_revision;
				owner.completed_primary_digest = work->binding_primary_digest;
				owner.phase = ManagedInitialRuntimeOwner::Phase::Result;
				app.pending_agent_run_cancellation_ids.erase(owner.expected.id);
				app.managed_subagent_cancellation_receipts.erase(owner.expected.id);
				std::erase(app.queued_agent_run_ids, owner.expected.id);
			}
			if (action == ManagedInitialRuntimeOwner::Action::Delivered)
				owner.phase = ManagedInitialRuntimeOwner::Phase::Done;
			owner.transition.reset();
			return true;
		}
		void RequestInitialStop(ManagedInitialRuntimeOwner& owner, std::string status, std::string code, std::string diagnostic)
		{
			owner.input_forbidden = true;
			owner.desired_status = std::move(status);
			owner.diagnostic_code = std::move(code);
			owner.diagnostic = std::move(diagnostic);
			owner.phase = ManagedInitialRuntimeOwner::Phase::Stopping;
		}
		bool BeginInitialParentResult(AppState& app, ManagedInitialRuntimeOwner& owner)
		{
			if (owner.transition || (owner.retired && !owner.retired->load(std::memory_order_acquire)) || InitialClock::now() < owner.retry_at || !InitialRuntimeOwned(app, owner, false))
				return false;
			const auto* root = ChatDomainService().FindChatById(app, owner.root_id);
			if (!root || !InitialRootIdle(app, *root, false) || AgentRunScheduler::HasPendingInitialMention(app, root->id))
				return false;
			auto work = native_history::TryCreateWorkerOwnedNativeRefresh<InitialTransition>();
			if (!work)
				return false;
			const auto retirement = std::make_shared<std::atomic<bool>>(false);
			work->retired = retirement;
			work->parent_before = *root;
			work->parent_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *root);
			owner.pending_parent_metadata = work->parent_metadata;
			work->expected = owner.expected;
			if (!root->messages_loaded)
			{
				work->cold = ChatDomainService().BeginColdHistoryPreparation(app, root->id);
				if (!work->cold)
					return false;
				work->cold_metadata = work->parent_metadata;
			}
			owner.retired = retirement;
			owner.transition = work;
			owner.action = ManagedInitialRuntimeOwner::Action::Parent;
			const auto root_path = owner.root;
			const auto delivery_id = InitialDeliveryId(owner);
			const bool followup = owner.resume_only;
			StartInitialPreparation(work,
			                        [root_path, delivery_id, followup](InitialTransition& work)
			                        {
				                        if (work.cold)
				                        {
					                        if (!ChatDomainService().LoadColdHistoryPreparation(*work.cold, work.error))
						                        return;
					                        work.parent_before = work.cold->snapshot;
					                        work.parent_metadata = ChatRepository::ChatMetadataFingerprint(root_path, work.parent_before);
				                        }
				                        work.parent_candidate = work.parent_before;
				                        const auto tool_id = "uam-agent-result-" + delivery_id;
				                        const bool present = std::ranges::any_of(work.parent_before.messages, [&](const Message& message) { return std::ranges::any_of(message.tool_calls, [&](const ToolCall& tool) { return tool.id == tool_id; }); });
				                        if (!present)
				                        {
					                        ChatDomainService().AddMessage(work.parent_candidate, MessageRole::Assistant, "Managed agent " + work.expected.agent_id + " " + work.expected.status + ".");
					                        ToolCall tool;
					                        tool.id = tool_id;
					                        tool.name = "uam_agent_result";
					                        tool.status = work.expected.status == "completed" ? "completed" : "failed";
					                        tool.args_json = nlohmann::json{{"runId", work.expected.id}}.dump();
					                        tool.result_text = nlohmann::json{{"runId", work.expected.id}, {"status", work.expected.status}, {"agentId", work.expected.agent_id}, {"providerId", work.expected.provider_id}, {"executionCapability", work.expected.execution_capability}, {"transcriptChatId", work.expected.transcript_chat_id}, {"startedAtEpochMs", work.expected.started_at_epoch_ms}, {"finishedAtEpochMs", work.expected.finished_at_epoch_ms}, {"startedAt", work.expected.started_at}, {"completedAt", work.expected.finished_at}, {"result", work.expected.result_excerpt}, {"diagnosticCode", work.expected.diagnostic_code}, {"diagnostic", work.expected.diagnostic}}.dump();
					                        if (followup)
					                        {
						                        auto result = nlohmann::json::parse(tool.result_text);
						                        result["turnId"] = work.expected.managed_continuation->current_turn_id;
						                        result["turnNumber"] = work.expected.managed_continuation->current_turn_number;
						                        tool.result_text = result.dump();
					                        }
					                        tool.is_sub_agent = true;
					                        tool.sub_agent_id = work.expected.id;
					                        tool.sub_agent_title = work.expected.agent_id;
					                        work.parent_candidate.messages.back().tool_calls.push_back(std::move(tool));
				                        }
				                        const auto revision = ChatRepository::ChatStorageRevision(root_path, work.parent_before.id);
				                        const auto saved = ChatRepository::LoadLocalChat(root_path, work.parent_before.id, true, nullptr, false);
				                        if (!revision || !saved || !InitialStoredChatMatches(root_path, *saved, work.parent_before) || ChatRepository::ChatStorageRevision(root_path, work.parent_before.id) != revision)
				                        {
					                        work.error = "The saved parent changed before managed result publication.";
					                        return;
				                        }
				                        work.root_prepared = ChatRepository::PrepareChatPublication(root_path, work.parent_candidate, ChatPublicationMode::UpdateExisting, *revision, &work.error);
			                        });
			return true;
		}

		bool PollInitialParentResult(AppState& app, ManagedInitialRuntimeOwner& owner)
		{
			const auto work = owner.transition;
			if (!work || owner.action != ManagedInitialRuntimeOwner::Action::Parent || !work->finished.load(std::memory_order_acquire))
				return false;
			if (work->cold && !work->cold_applied)
			{
				const auto* root = ChatDomainService().FindChatById(app, owner.root_id);
				const bool owned_cold = root && InitialRuntimeOwned(app, owner, false) && InitialRootIdle(app, *root, false, work->cold->ownership.get()) && ChatRepository::ChatMetadataFingerprint(app.data_root, *root) == work->cold_metadata;
				if (!owned_cold || !work->error.empty() || !ChatDomainService().CompleteColdHistoryPreparation(app, *work->cold))
				{
					(void)ChatDomainService().ConsumeBranchPreparation(app, owner.root_id, work->cold->ownership);
					owner.phase = ManagedInitialRuntimeOwner::Phase::Result;
					owner.transition.reset();
					return true;
				}
				work->cold_applied = true;
				work->parent_before = *ChatDomainService().FindChatById(app, owner.root_id);
				work->parent_metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, work->parent_before);
				owner.pending_parent_metadata = work->parent_metadata;
			}
			if (owner.parent_result_uncertain && !owner.parent_retry_prepared)
			{
				if (app.managed_initial_shutdown)
				{
					owner.transition.reset();
					return true;
				}
				if (InitialClock::now() < owner.retry_at)
					return false;
				const auto root_path = owner.root;
				owner.parent_retry_prepared = true;
				StartInitialPreparation(work,
				                        [root_path](InitialTransition& work)
				                        {
					                        work.root_prepared.reset();
					                        work.error.clear();
					                        const auto revision = ChatRepository::ChatStorageRevision(root_path, work.parent_before.id);
					                        const auto saved = ChatRepository::LoadLocalChat(root_path, work.parent_before.id, true, nullptr, false);
					                        // Confirmation retries may see either exact publication outcome, never a different parent.
					                        const bool before = saved && InitialStoredChatMatches(root_path, *saved, work.parent_before);
					                        const bool candidate = saved && InitialStoredChatMatches(root_path, *saved, work.parent_candidate);
					                        if (!revision || (!before && !candidate) || ChatRepository::ChatStorageRevision(root_path, work.parent_before.id) != revision)
					                        {
						                        work.error = "Managed result publication is still unconfirmed. Its exact parent record was retained.";
						                        return;
					                        }
					                        work.root_prepared = ChatRepository::PrepareChatPublication(root_path, work.parent_candidate, ChatPublicationMode::UpdateExisting, *revision, &work.error);
				                        });
				return false;
			}
			const auto owned = [&]
			{
				const auto* root = ChatDomainService().FindChatById(app, owner.root_id);
				return InitialRuntimeOwned(app, owner, false) && root && InitialRootIdle(app, *root, true, nullptr, owner.parent_result_uncertain) && !app.managed_initial_mentions.contains(owner.root_id) && root->messages == work->parent_before.messages && ChatRepository::ChatMetadataFingerprint(app.data_root, *root) == work->parent_metadata;
			};
			ChatSession committed = work->parent_candidate;
			ChatPublicationResult result;
			if (work->root_prepared && work->error.empty() && owned())
			{
				if (!owner.parent_stage_hook_used)
				{
					owner.parent_stage_hook_used = true;
					result = ChatRepository::PublishPreparedChatPublication(work->root_prepared, owned, &committed, owner.parent_stage_hook_for_tests);
				}
				else
					result = ChatRepository::PublishPreparedChatPublication(work->root_prepared, owned, &committed);
			}
			if (result.state == ChatPublicationState::Committed)
			{
				committed.metadata_incarnation = owner.root_incarnation;
				auto* current = ChatDomainService().FindChatById(app, owner.root_id);
				committed.managed_subagent_revision = std::max(committed.managed_subagent_revision, current->managed_subagent_revision);
				*current = std::move(committed); // Same owner stack, before delivered CAS.
				owner.parent_result_acked = true;
				owner.parent_result_uncertain = false; // Primary ACK precedes delivered CAS.
			}
			owner.retry_at = InitialClock::now() + std::chrono::milliseconds(500);
			if (result.state == ChatPublicationState::Uncertain || owner.parent_result_uncertain)
			{
				owner.parent_result_uncertain = true;
				owner.parent_retry_prepared = false;
				return true; // Retain the exact worker-owned candidate; only bookkeeping confirmation may retry.
			}
			if (result.state != ChatPublicationState::Committed)
				owner.phase = ManagedInitialRuntimeOwner::Phase::Result;
			owner.transition.reset();
			return true;
		}
		bool PollInitialRuntimes(AppState& app, int64_t now_epoch_ms, int64_t now_steady_ms)
		{
			bool changed = false;
			std::vector<std::string> ids;
			for (const auto& [id, owner] : app.managed_initial_runtime_owners)
				ids.push_back(id);
			for (const auto& id : ids)
			{
				const auto found = app.managed_initial_runtime_owners.find(id);
				if (found == app.managed_initial_runtime_owners.end())
					continue;
				auto owner = found->second;
				if (!InitialRuntimeOwned(app, *owner, false))
				{
					ForbidInitialInput(*owner);
					if (owner->transition && owner->transition->finished.load(std::memory_order_acquire))
						owner->transition.reset();
					const auto* session = FindAcpSessionForChat(app, owner->transcript_id);
					if (session && session == owner->session && session->managed_initial_owner == owner && session->running && !owner->process.empty() && session->last_process_id == owner->process && acp_detail::AcpProcessHandleLabel(*session) == owner->process && !owner->orphan_stop_transferred)
					{
						// The actual original process is owned even though its chat namespace was replaced.
						// Empty chat ID prevents all replacement lookup/history finalization.
						auto* original = FindAcpSessionForChat(app, owner->transcript_id);
						QueueAcpProcessStop(app, *original, {}, AcpStopPurpose::Interrupt, true);
						auto& stop = app.acp_process_stop_tasks.back();
						AgentRunScheduler::InitialProcessStopQueued(*original, stop);
						owner->orphan_stop_transferred = true;
						original->running = false;
						original->reconnect_pending = false;
					}
					continue; // Retain bookkeeping; never publish or Stop through a replacement transcript/data root.
				}
				if (!InitialRuntimeOwned(app, *owner) && !IsTerminal(owner->expected.status))
					ForbidInitialInput(*owner);
				if (app.managed_initial_shutdown && !IsTerminal(owner->expected.status))
					RequestInitialStop(*owner, "interrupted", "manager_shutdown", "UAM stopped this managed initial run during shutdown.");
				if (app.pending_agent_run_cancellation_ids.contains(id) && !IsTerminal(owner->expected.status))
					RequestInitialStop(*owner, "cancelled", "user_cancelled", "The user cancelled this agent run.");
				if (owner->transition)
				{
					changed = PollInitialParentResult(app, *owner) || changed;
					if (owner->transition && owner->action != ManagedInitialRuntimeOwner::Action::Parent)
						changed = PollInitialTransition(app, *owner) || changed;
					if (owner->transition)
						continue;
				}
				if (owner->retired && !owner->retired->load(std::memory_order_acquire))
					continue;
				if (owner->uncertain_candidate)
				{
					StartInitialReconciliation(*owner);
					continue;
				}
				if (IsTerminal(owner->expected.status))
				{
					if (InitialResultDelivered(*owner))
					{
						app.pending_agent_run_cancellation_ids.erase(id);
						app.managed_subagent_cancellation_receipts.erase(id);
						app.agent_run_deadline_steady_ms.erase(id);
						if (InitialStopped(app, *owner))
						{
							RetainFollowupWitness(app, *owner);
							EraseRuntimeChat(app, owner->transcript_id);
							app.managed_initial_runtime_owners.erase(id);
						}
						changed = true;
						continue;
					}
					if (app.managed_initial_shutdown)
						continue; // Terminal ledger is sufficient for shutdown; parent result waits for an ordinary owner turn.
					if (owner->parent_result_acked)
					{
						auto candidate = InitialCandidate(*owner);
						if (!owner->resume_only) candidate.root_result_delivered = true;
						candidate.managed_continuation->turns.back().result_delivered = true;
						(void)BeginInitialTransition(app, *owner, std::move(candidate), ManagedInitialRuntimeOwner::Action::Delivered);
						continue;
					}
					if (owner->phase == ManagedInitialRuntimeOwner::Phase::Result)
					{
						if (InitialResultAttempts(*owner) >= 3)
							continue;
						auto candidate = InitialCandidate(*owner);
						++candidate.managed_continuation->turns.back().result_delivery_attempts;
						if (!owner->resume_only) candidate.root_result_delivery_attempts = candidate.managed_continuation->turns.back().result_delivery_attempts;
						if (BeginInitialTransition(app, *owner, std::move(candidate), ManagedInitialRuntimeOwner::Action::Attempts))
							owner->phase = ManagedInitialRuntimeOwner::Phase::Stopping;
						continue;
					}
					(void)BeginInitialParentResult(app, *owner);
					continue;
				}
				if (!owner->settled)
				{
					const auto* setup = FindAcpSessionForChat(app, owner->transcript_id);
					if (setup && setup == owner->session && setup->managed_initial_owner == owner && setup->lifecycle_state == acp_detail::kAcpLifecycleError)
						ForbidInitialInput(*owner, strings::NonEmptyOrFallback(setup->last_error, "The managed native setup failed. Input was not replayed."));
				}
				if (!owner->desired_status.empty())
				{
					owner->phase = ManagedInitialRuntimeOwner::Phase::Stopping;
					owner->input_forbidden = true;
					if (const auto* chat = ChatDomainService().FindChatById(app, owner->transcript_id))
						owner->output = owner->resume_only && owner->row < 0 ? std::string{} : ResultExcerpt(*chat, owner->resume_only ? static_cast<std::size_t>(owner->row + 1) : 0);
					const auto* stopped_session = FindAcpSessionForChat(app, owner->transcript_id);
					const auto* stopped_chat = ChatDomainService().FindChatById(app, owner->transcript_id);
					// Finalize once, then preserve its exact canonical save while detached terminal CAS is prepared.
					if (!InitialStopped(app, *owner) || (stopped_session && (!stopped_chat || !InitialReadOnlyStoppedLeaseOwned(app, *owner, *stopped_session, *stopped_chat))))
						(void)StopAcpSession(app, owner->transcript_id);
					if (!InitialStopped(app, *owner) || app.pending_chat_save_at_by_chat_id.contains(owner->transcript_id))
						continue;
					auto candidate = InitialCandidate(*owner);
					candidate.status = owner->desired_status;
					candidate.finished_at = time::TimestampNow();
					candidate.updated_at = candidate.finished_at;
					candidate.finished_at_epoch_ms = now_epoch_ms;
					candidate.result_excerpt = owner->output;
					candidate.diagnostic_code = owner->diagnostic_code;
					candidate.diagnostic = owner->diagnostic;
					auto& turn = candidate.managed_continuation->turns.back();
					turn.status = candidate.status == "completed" ? ManagedRunTurnStatus::Completed : candidate.status == "cancelled" ? ManagedRunTurnStatus::Cancelled : candidate.status == "interrupted" ? ManagedRunTurnStatus::Interrupted : ManagedRunTurnStatus::Failed;
					turn.finished_at_epoch_ms = candidate.finished_at_epoch_ms;
					turn.diagnostic_code = candidate.diagnostic_code;
					(void)BeginInitialTransition(app, *owner, std::move(candidate), ManagedInitialRuntimeOwner::Action::Terminal);
					continue;
				}
				if (owner->phase == ManagedInitialRuntimeOwner::Phase::Queued)
				{
					const auto running = RunningAgentSlots(app);
					if (!owner->completion_acked || running >= kMaxRunningAgents)
						continue;
					auto* routed = ChatDomainService().FindChatById(app, owner->transcript_id);
					const ChatSession before_routing = *routed;
					const auto routing = AdmitExecutionRouting(app, *routed);
					if (routing.status == ExecutionRoutingStatus::Waiting || routing.status == ExecutionRoutingStatus::Invalid)
						continue;
					if (routing.status == ExecutionRoutingStatus::Assigned && routed->routing_mode == "automatic")
					{
						ChatSession admitted = before_routing;
						admitted.execution_host_id = routing.assignment.host_id;
						admitted.folder_id = routing.assignment.destination_folder_id;
						admitted.workspace_directory = routing.assignment.destination_directory;
						if (!admitted.workspace_worktree_directory.empty())
							admitted.workspace_worktree_directory = routing.assignment.destination_directory;
						admitted.routing_source_directory = routed->routing_source_directory;
						admitted.routing_assigned_connection_identity = routing.assignment.connection_identity;
						admitted.routing_assigned_provider_configuration_identity = routing.assignment.provider_configuration_identity;
						// The synchronous routing service is the sole authority for these exact saved assignment fields.
						// Captured model/options/policy are never replaced by host defaults in this initial slice.
						if (InitialChildPolicy(admitted) != InitialChildPolicy(*routed) || !InitialRuntimeOwned(app, *owner, false))
						{
							ForbidInitialInput(*owner, "Managed routing changed captured model or policy. No task input was sent.");
							continue;
						}
						owner->child_policy = InitialChildPolicy(*routed);
						owner->settings = InitialConfiguration(app, *routed);
					}
					if (!InitialRuntimeOwned(app, *owner))
					{
						ForbidInitialInput(*owner, "The managed initial route changed. No task input was sent.");
						continue;
					}
					if (ProviderCircuitOpen(app, owner->expected.provider_id, now_epoch_ms))
					{
						ForbidInitialInput(*owner, "Managed launches are paused after repeated provider failures.");
						continue;
					}
					auto candidate = InitialCandidate(*owner);
					candidate.status = "running";
					candidate.started_at = candidate.updated_at;
					candidate.started_at_epoch_ms = now_epoch_ms;
					candidate.deadline_at_epoch_ms = now_epoch_ms + kMaxRunElapsedMs;
					auto& turn = candidate.managed_continuation->turns.back();
					turn.status = ManagedRunTurnStatus::Running;
					turn.dispatch_attempted = true;
					turn.started_at_epoch_ms = now_epoch_ms;
					if (BeginInitialTransition(app, *owner, std::move(candidate), ManagedInitialRuntimeOwner::Action::Running))
						app.agent_run_deadline_steady_ms[id] = now_steady_ms + kMaxRunElapsedMs;
					continue;
				}
				const auto deadline = app.agent_run_deadline_steady_ms.find(id);
				if (deadline != app.agent_run_deadline_steady_ms.end() && now_steady_ms >= deadline->second)
				{
					RequestInitialStop(*owner, "failed", "wall_clock_deadline_exceeded", "The managed agent run exceeded its 30-minute elapsed-time limit.");
					continue;
				}
				auto* transcript = ChatDomainService().FindChatById(app, owner->transcript_id);
				if (owner->phase == ManagedInitialRuntimeOwner::Phase::Starting)
				{
					auto& session = acp_detail::EnsureAcpSessionForChat(app, *transcript);
					if (owner->session && owner->session != &session)
					{
						ForbidInitialInput(*owner);
						continue;
					}
					owner->session = &session;
					session.managed_initial_owner = owner;
					if (session.pending_local_launch || session.worktree_launch_pending || session.processing || !session.queued_user_prompts.empty() || session.reconnect_pending)
						continue;
					owner->send_authorized = true;
					owner->send_called = true;
					std::string error;
					const bool sent = SendAcpPrompt(app, owner->transcript_id, InitialTask(*owner), {}, {}, false, &error);
					owner->send_authorized = false;
					if (!sent && !session.pending_local_launch && !session.worktree_launch_pending)
						ForbidInitialInput(*owner, strings::NonEmptyOrFallback(error, "The managed initial process could not start."));
					std::erase(app.queued_agent_run_ids, id);
					changed = true;
					continue;
				}
				if (owner->phase == ManagedInitialRuntimeOwner::Phase::Row)
				{
					if (owner->saved_serial != owner->serial || owner->saved_row != owner->row || owner->saved_row_created != owner->row_created)
						continue;
					auto candidate = InitialCandidate(*owner);
					candidate.expected_turn_serial = owner->serial;
					auto& turn = candidate.managed_continuation->turns.back();
					turn.expected_turn_serial = owner->serial;
					turn.first_user_message_index = owner->row;
					turn.user_message_created_at = owner->row_created;
					(void)BeginInitialTransition(app, *owner, std::move(candidate), ManagedInitialRuntimeOwner::Action::Row);
					continue;
				}
				const auto* session = FindAcpSessionForChat(app, owner->transcript_id);
				if (session && !session->running && !session->local_stop_pending && !session->remote_stop_pending && session->managed_launch_attempted && !owner->settled)
				{
					ForbidInitialInput(*owner, strings::NonEmptyOrFallback(session->last_error, "The provider exited before the owned initial turn settled."));
					RecordProviderCrash(app, owner->expected.provider_id, now_epoch_ms);
				}
			}
			return changed;
		}
	} // namespace

	ManagedFollowupState AgentRunScheduler::GetFollowupState(const AppState& app, const std::string& root_id, const std::string& run_id)
	{
		ManagedFollowupState result;
		result.code = FollowupLiveCode(app, root_id, run_id);
		result.can_send = result.code == ManagedFollowupCode::None;
		if (const auto* run = FindRun(app, run_id); result.code != ManagedFollowupCode::ScopeChanged && run && run->managed_continuation)
		{
			result.current_turn_id = run->managed_continuation->current_turn_id;
			result.current_turn_number = run->managed_continuation->current_turn_number;
		}
		return result; // Bounded no-I/O eligibility, never disk/native/dispatch authority.
	}

	ManagedFollowupAdmission AgentRunScheduler::TryBeginFollowup(AppState& app, const std::string& root_id, const std::string& run_id, const ManagedFollowupRequest& request, std::function<bool(const ManagedFollowupSubmitResult&)> completion, ManagedFollowupCode* rejection)
	{
		const auto reject = [&](ManagedFollowupCode code) { if (rejection) *rejection = code; return ManagedFollowupAdmission::Rejected; };
		const auto scope = FollowupScopeCode(app, root_id, run_id);
		if (scope != ManagedFollowupCode::None) return reject(scope);
		if (!completion || !uuid::IsCanonicalUuid(request.client_request_id) || !uuid::IsCanonicalUuid(request.after_turn_id) || request.text.empty() || request.text.size() > 32U * 1024U || request.text.find('\0') != std::string::npos || strings::IsBlank(request.text) || request.continuity != ManagedRunContinuity::Native || request.recovery_decision != ManagedRunRecoveryDecision::None || !request.retry_of_turn_id.empty()) return reject(ManagedFollowupCode::InvalidRequest);
		const auto* run = FindRun(app, run_id);
		if (!run || run->root_chat_id != root_id || std::ranges::count(app.agent_runs, run_id, &AgentRun::id) != 1) return reject(ManagedFollowupCode::ScopeChanged);
		bool replay = false;
		if (run && run->managed_continuation)
		{
			const auto previous = std::ranges::find(run->managed_continuation->turns, request.client_request_id, &ManagedRunTurnReceipt::client_request_id);
			if (previous != run->managed_continuation->turns.end())
			{
				if (!FollowupRequestMatches(*previous, request)) return reject(ManagedFollowupCode::IdempotencyConflict);
				replay = true;
			}
		}
		const auto code = FollowupLiveCode(app, root_id, run_id, replay);
		if (code != ManagedFollowupCode::None) return reject(code);
		if (!replay && request.after_turn_id != run->managed_continuation->current_turn_id) return reject(ManagedFollowupCode::InvalidRequest);
		std::size_t text_bytes = request.text.size();
		for (const auto& turn : run->managed_continuation->turns) text_bytes += turn.text.size();
		if (!replay && text_bytes > 128U * 1024U) return reject(ManagedFollowupCode::ReceiptLimit);
		auto work = native_history::TryCreateWorkerOwnedNativeRefresh<FollowupPreparation>();
		if (!work) return reject(ManagedFollowupCode::Busy);
		const auto* root = ChatDomainService().FindChatById(app, root_id);
		auto task = std::make_shared<PendingManagedFollowup>();
		task->root = app.data_root; task->root_id = root_id; task->run_id = run_id; task->transcript_id = run->transcript_chat_id;
		task->incarnation = root->metadata_incarnation; task->root_created = root->created_at; task->root_birth = root->managed_followup_incarnation;
		task->metadata = ChatRepository::ChatMetadataFingerprint(app.data_root, *root); task->settings = InitialConfiguration(app, *root);
		task->request = request; task->completion = std::move(completion); task->replay = replay; task->work = work;
		if (!replay)
		{
			task->source = app.managed_followup_completed_owners.at(run_id);
			work->predecessor_revision = task->source->primary_revision;
			work->predecessor_digest = task->source->primary_digest;
		}
		work->retired = task->retired; work->root = task->root; work->parent = *root; work->expected = *run; work->candidate = *run;
		if (const auto* child = ChatDomainService().FindChatById(app, task->transcript_id)) work->live_transcript = *child;
		if (!replay)
		{
			auto& candidate = work->candidate;
			auto& value = *candidate.managed_continuation;
			ManagedRunTurnReceipt turn;
			turn.turn_id = turn.delivery_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
			if (!uuid::IsCanonicalUuid(turn.turn_id)) return reject(ManagedFollowupCode::PersistenceFailed);
			turn.turn_number = static_cast<std::uint32_t>(value.turns.size() + 1);
			turn.client_request_id = request.client_request_id; turn.after_turn_id = request.after_turn_id; turn.text = request.text;
			turn.principal = {ManagedRunPrincipalKind::UserRoot, root_id, value.owner.root_birth_id, {}, {}};
			turn.result_required = true;
			value.current_turn_id = turn.turn_id; value.current_turn_number = turn.turn_number; ++value.state_revision;
			value.turns.push_back(std::move(turn));
			candidate.status = "queued"; candidate.expected_turn_serial = 0;
			candidate.started_at.clear(); candidate.finished_at.clear(); candidate.started_at_epoch_ms = candidate.finished_at_epoch_ms = candidate.deadline_at_epoch_ms = 0;
			candidate.result_excerpt.clear(); candidate.diagnostic_code.clear(); candidate.diagnostic.clear(); candidate.updated_at = time::TimestampNow();
		}
		app.managed_followups.emplace(run_id, task);
		StartInitialPreparation(work, [request, replay](FollowupPreparation& work) { PrepareFollowup(work, request, replay); });
		if (rejection) *rejection = ManagedFollowupCode::None;
		return ManagedFollowupAdmission::Pending;
	}

	bool AgentRunScheduler::BindCurrentManagedPrompt(const AppState& app, const ChatSession& chat, AcpQueuedUserPromptState& queued, std::string* error_out)
	{
		const auto owner = InitialOwner(app, chat);
		if (!owner || !owner->resume_only)
		{
			const auto* run = FindRun(app, chat.agent_run_id);
			if (run && run->managed_continuation && run->managed_continuation->current_turn_number > 1) { if (error_out) *error_out = "The current managed follow-up owner is unavailable."; return false; }
			return true; // Existing ordinary/initial policy binding is unchanged.
		}
		const auto& turn = InitialCurrentTurn(*owner);
		const auto& run = owner->expected;
		const bool allowed = InitialSendAllowed(app, chat, turn.text) && queued.text == strings::Trim(turn.text) && queued.attachments.empty() && queued.markdown_store_files.empty() && queued.markdown_store_prompt_blocks.empty() && !queued.goal_mode && queued.goal_id.empty() && !queued.computer_use_mode && queued.uam_agent_id == run.agent_id && queued.uam_agent_definition_hash == run.definition_hash && queued.uam_agent_definition_snapshot == run.definition_snapshot && queued.uam_agent_instructions == run.definition_instructions && queued.uam_agent_skills == run.skills_snapshot && queued.uam_agent_delegates == run.delegates_snapshot && queued.uam_agent_workspace_access == run.effective_workspace_access && queued.uam_agent_execution_capability == run.execution_capability;
		if (!allowed)
		{
			if (error_out) *error_out = "The current managed prompt lost its captured text or policy.";
			return false;
		}
		queued.id = turn.turn_id;
		queued.text = turn.text; // Restore exact authored bytes before any subsequent policy, row or save capture.
		queued.managed_turn = ManagedRunPromptIdentity{run.id, turn.turn_id, turn.turn_number, run.managed_continuation->execution->native_session_id, ManagedRunContinuity::Native};
		return true;
	}

	bool AgentRunScheduler::ManagedResumeIdentityAllowed(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string_view native)
	{
		const auto owner = InitialOwner(app, chat);
		if (!owner || !owner->resume_only)
		{
			const auto* run = FindRun(app, chat.agent_run_id);
			if (run && run->managed_continuation && run->managed_continuation->current_turn_number > 1) { session.last_error = "The current managed native resume owner is unavailable."; return false; }
			return true;
		}
		const bool allowed = session.managed_initial_owner == owner && owner->session == &session && InitialRuntimeOwned(app, *owner) && owner->expected.managed_continuation->execution && uuid::IsCanonicalUuid(native) && native == owner->expected.managed_continuation->execution->native_session_id && chat.native_session_id == native && (session.session_id.empty() || session.session_id == native) && (owner->process.empty() || session.last_process_id == owner->process);
		if (!allowed)
		{
			owner->input_forbidden = true;
			if (!IsTerminal(owner->expected.status))
			{
				owner->desired_status = "failed"; owner->diagnostic_code = "native_resume_identity_changed";
				owner->diagnostic = "The provider did not retain the exact saved native conversation. No input was replayed.";
			}
		}
		if (!allowed) session.last_error = "The exact saved managed native resume identity is unavailable.";
		return allowed;
	}

	bool AgentRunScheduler::InitialForceSaveAllowed(const AppState& app, std::string_view chat_id)
	{
		if (!app.managed_initial_shutdown)
			return true;
		if (HasPendingInitialMention(app, chat_id))
			return false;
		for (const auto& [id, owner] : app.managed_initial_runtime_owners)
		{
			if (!owner || owner->root != app.data_root || (owner->root_id != chat_id && owner->transcript_id != chat_id))
				continue;
			const auto* root = ChatDomainService().FindChatById(app, owner->root_id);
			const auto* child = ChatDomainService().FindChatById(app, owner->transcript_id);
			if (!root || !child || root->metadata_incarnation != owner->root_incarnation || child->metadata_incarnation != owner->transcript_incarnation)
				continue;
			if (!InitialStopped(app, *owner) || !IsTerminal(owner->expected.status) || owner->transition || owner->uncertain_candidate || owner->parent_result_uncertain || (owner->retired && !owner->retired->load(std::memory_order_acquire)))
				return false;
		}
		return true;
	}

	bool AgentRunScheduler::QuiesceInitialForShutdown(AppState& app)
	{
		app.managed_initial_shutdown = true;
		app.managed_followup_completed_owners.clear();
		for (const auto& [id, task] : app.managed_followups)
		{
			if (!task) continue;
			task->cancelled = true;
			if (task->work) task->work->cancel.store(true, std::memory_order_release);
		}
		for (const auto& [id, task] : app.managed_initial_mentions)
		{
			if (!task)
				continue;
			task->cancelled = true;
			if (task->work)
				task->work->cancel.store(true, std::memory_order_release);
		}
		for (const auto& [id, owner] : app.managed_initial_runtime_owners)
			if (owner && !IsTerminal(owner->expected.status))
				RequestInitialStop(*owner, "interrupted", "manager_shutdown", "UAM stopped this managed initial run during shutdown.");
		const auto stop_deadline = InitialClock::now() + std::chrono::seconds(2);
		bool settled = false;
		for (;;)
		{
			(void)PollFollowupCreations(app);
			(void)PollInitialCreations(app);
			(void)PollInitialRuntimes(app, time::SystemEpochMillisecondsNow(), SteadyMillisecondsNow());
			(void)PollCompletedAcpProcessStops(app, nullptr);
			FinalizePendingAcpRemoteStopsForExit(app);
			(void)FlushPendingChatSaves(app, false);
			settled = app.managed_initial_mentions.empty() && app.managed_followups.empty();
			bool workers = std::ranges::any_of(app.managed_initial_mentions, [](const auto& entry) { return entry.second && (entry.second->work || !entry.second->retired->load(std::memory_order_acquire)); });
			workers = workers || std::ranges::any_of(app.managed_followups, [](const auto& entry)
			{
				const auto& task = entry.second;
				return task && ((task->work && !task->work->finished.load(std::memory_order_acquire)) || (task->phase == PendingManagedFollowup::Phase::Cleaning && !task->retired->load(std::memory_order_acquire)));
			});
			for (const auto& [id, owner] : app.managed_initial_runtime_owners)
			{
				if (!owner)
					continue;
				workers = workers || owner->transition || (owner->retired && !owner->retired->load(std::memory_order_acquire));
				settled = settled && InitialStopped(app, *owner) && IsTerminal(owner->expected.status) && !owner->uncertain_candidate;
			}
			if (settled || (InitialClock::now() >= stop_deadline && !workers))
				break;
			if (InitialClock::now() >= stop_deadline)
			{
				// Stop scheduling new bookkeeping retries; finish and retire every already admitted worker.
				for (const auto& [id, owner] : app.managed_initial_runtime_owners)
					if (owner)
						owner->retry_at = InitialClock::time_point::max();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return settled; // False is quiesced/uncertain, never a persisted terminal claim.
	}

	bool AgentRunScheduler::Enqueue(AppState& app, const std::string& root_chat_id,
	                                const std::string& parent_run_id, const std::string& agent_id,
	                                const std::string& task, std::string* run_id_out,
	                                std::string* error_out, const std::string& provider_id,
	                                const std::string& model_id, bool additional_read_only_ceiling)
	{
		ChatSession* root_chat = ChatDomainService().FindChatById(app, uam::strings::Trim(root_chat_id));
		if (root_chat && HasPendingInitialMention(app, root_chat->id))
		{
			if (error_out)
				*error_out = "Wait for the delegated request to finish saving before starting another agent.";
			return false;
		}
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
		if (parent != nullptr)
		{
			const AcpSessionState* session = FindAcpSessionForChat(app, parent->transcript_chat_id);
			if (app.pending_agent_run_cancellation_ids.contains(parent->id) || app.managed_subagent_cancellation_receipts.contains(parent->id) || (session != nullptr && session->managed_cancellation_pending))
			{
				if (error_out != nullptr)
					*error_out = "The parent agent run is stopping and cannot delegate new work.";
				return false;
			}
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

		const std::filesystem::path parent_workspace = uam::paths::ResolveWorkspaceRootPath(app, *root_chat);
		const std::filesystem::path controller_workspace = uam::paths::ResolveControllerWorkspaceRootPath(app, *root_chat);
		AgentDefinitionCatalog agents = AgentDefinitionService::Load(app.data_root, controller_workspace);
		const AgentDefinition* definition = FindAgent(agents, requested_agent);
		if (definition == nullptr && uam::paths::IsControllerLocalWorkspace(*root_chat) &&
		    !root_chat->workspace_source_directory.empty() &&
		    uam::mcp_server_config::WorkspaceKey(root_chat->workspace_source_directory) !=
		        uam::mcp_server_config::WorkspaceKey(uam::paths::Utf8PathString(controller_workspace)))
		{
			agents = AgentDefinitionService::Load(app.data_root, uam::paths::PathFromUtf8(root_chat->workspace_source_directory));
			definition = FindAgent(agents, requested_agent);
		}
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

		const std::string selected_provider = provider_ids::NormalizeCliProviderAliasOrSelf(
		    uam::strings::NonEmptyOrFallback(uam::strings::Trim(provider_id), root_chat->provider_id));
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
		// A validated caller's captured restriction can only narrow the current root/definition policy.
		const bool read_only = additional_read_only_ceiling || definition->workspace_access == "read" || root_agent_read_only ||
		                       approval_modes::AppApprovalModeOrEmpty(root_chat->approval_mode) == approval_modes::kPlanApprovalMode ||
		                       (parent != nullptr && parent->effective_workspace_access == "read");

		ChatSession transcript = ChatDomainService().CreateNewChat(root_chat->folder_id, selected_provider);
		transcript.title = "Agent: " + requested_agent;
		transcript.execution_host_id = root_chat->execution_host_id;
		transcript.workspace_directory = uam::paths::Utf8PathString(parent_workspace);
		// Keep the source only as a definition lookup fallback. The child does not own or manage the parent's worktree.
		transcript.workspace_source_directory = root_chat->workspace_source_directory;
		transcript.routing_mode = root_chat->routing_mode;
		if (transcript.routing_mode == "automatic")
		{
			transcript.routing_source_folder_id = root_chat->routing_source_folder_id;
			transcript.routing_source_directory = root_chat->routing_source_directory;
			const ChatFolder* source = ChatDomainService().FindFolderById(app, transcript.routing_source_folder_id);
			if (source != nullptr)
			{
				transcript.folder_id = source->id;
				transcript.execution_host_id = source->execution_host_id;
				transcript.workspace_directory = transcript.routing_source_directory;
			}
		}

		const bool same_provider = selected_provider == provider_ids::NormalizeCliProviderAliasOrSelf(root_chat->provider_id);
		transcript.model_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(model_id), same_provider ? root_chat->model_id : std::string{});
		if (same_provider)
		{
			// Launch-only model options belong to the concrete model they were selected for.
			if (!ProviderRuntimeRegistry::ResolveById(selected_provider).ModelOptionsRequireRestart() || transcript.model_id == root_chat->model_id)
				transcript.reasoning_effort = root_chat->reasoning_effort;
			transcript.service_tier = root_chat->service_tier;
			transcript.service_tier_explicit = root_chat->service_tier_explicit;
		}
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
		// Reacquire after vector growth; this is a UI signal, not admission or persisted state.
		if (ChatSession* root = ChatDomainService().FindChatById(app, run.root_chat_id))
			++root->managed_subagent_revision;
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

		// Enqueue may grow app.chats and invalidate an ID borrowed from its parent.
		const std::string root_id = root_chat_id;
		ChatSession* root_chat = ChatDomainService().FindChatById(app, root_id);
		if (root_chat == nullptr || !root_chat->agent_run_id.empty()) return true;
		const std::string agent_id = text.substr(1, end - 1);
		const AgentDefinitionCatalog catalog = AgentDefinitionService::Load(
		    app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(app, *root_chat));
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

		std::string warning;
		if (!ChatRepository::HydrateChatMessages(app.data_root, *root_chat, &warning))
		{
			if (error_out != nullptr) *error_out = "Could not load the @agent parent chat: " + warning;
			return false;
		}
		std::string run_id;
		if (!Enqueue(app, root_id, {}, agent_id, task, &run_id, error_out))
			return false;
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
		root_chat = ChatDomainService().FindChatById(app, root_id);
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
		if (interrupted && interrupted->managed_continuation)
		{
			if (error_out)
				*error_out = "This initial managed record cannot be replayed through legacy Resume.";
			return false;
		}
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

		ChatSession transcript = *old_transcript;
		std::string hydrate_error;
		if (!ChatRepository::HydrateChatMessages(app.data_root, transcript, &hydrate_error))
		{
			if (error_out != nullptr) *error_out = uam::strings::NonEmptyOrFallback(
			    hydrate_error, "The interrupted run's durable transcript is unavailable.");
			return false;
		}
		const ChatSession fresh_identity = ChatDomainService().CreateNewChat(
		    transcript.folder_id, interrupted->provider_id);
		transcript.id = fresh_identity.id;
		transcript.folder_id = root_chat->folder_id;
		transcript.execution_host_id = root_chat->execution_host_id;
		transcript.workspace_directory = root_chat->workspace_directory;
		transcript.native_session_id.clear();
		transcript.routing_mode = root_chat->routing_mode;
		transcript.routing_source_folder_id = root_chat->routing_source_folder_id;
		transcript.routing_source_directory = root_chat->routing_source_directory;
		transcript.routing_assigned_connection_identity.clear();
		transcript.routing_assigned_provider_configuration_identity.clear();
		transcript.routing_reservation_pending = false;
		if (transcript.routing_mode == "automatic")
		{
			const ChatFolder* source = ChatDomainService().FindFolderById(app, transcript.routing_source_folder_id);
			if (source != nullptr)
			{
				transcript.folder_id = source->id;
				transcript.execution_host_id = source->execution_host_id;
				transcript.workspace_directory = transcript.routing_source_directory;
				transcript.workspace_worktree_directory.clear();
			}
		}
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
		run.finished_at_epoch_ms = 0;
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
		if (ChatSession* root = ChatDomainService().FindChatById(app, run.root_chat_id))
			++root->managed_subagent_revision;
		if (new_run_id_out != nullptr) *new_run_id_out = run.id;
		return true;
	}

	static bool PollAt(AppState& app, int64_t now_epoch_ms, int64_t now_steady_ms)
	{
		PruneFollowupWitnesses(app);
		bool changed = PollFollowupCreations(app);
		changed = PollInitialCreations(app) || changed;
		changed = PollInitialRuntimes(app, now_epoch_ms, now_steady_ms) || changed;
		const std::vector<std::string> pending_cancellations(app.pending_agent_run_cancellation_ids.begin(), app.pending_agent_run_cancellation_ids.end());
		for (const std::string& id : pending_cancellations)
		{
			if (app.pending_agent_run_cancellation_ids.contains(id) && ManagedSubagentControlService::ValidateCancellationOwnership(app, id) && AgentRunScheduler::CancelTree(app, id))
				changed = true;
		}
		for (AgentRun& run : app.agent_runs)
		{
			if (!run.managed_continuation && DeliverRootResult(app, run))
				changed = true;
		}
		for (AgentRun& run : app.agent_runs)
		{
			if (run.managed_continuation || run.status != "running" || app.pending_agent_run_cancellation_ids.contains(run.id))
				continue;
			AcpSessionState* session = FindAcpSessionForChat(app, run.transcript_chat_id);
			const auto steady_deadline = app.agent_run_deadline_steady_ms.find(run.id);
			const bool deadline_expired = steady_deadline != app.agent_run_deadline_steady_ms.end()
				? now_steady_ms >= steady_deadline->second
				: run.deadline_at_epoch_ms > 0 && now_epoch_ms >= run.deadline_at_epoch_ms;
			// Explicit pending cancellation keeps precedence through the loop/skip above.
			// An expired deadline precedes later settlement and generic Stop retry flags.
			if (deadline_expired)
			{
				if (app.pending_chat_save_at_by_chat_id.contains(run.transcript_chat_id)) continue;
				const std::string status_before_stop = app.status_line;
				const auto stop_projection = [&]()
				{
					const auto* current = FindAcpSessionForChat(app, run.transcript_chat_id);
					return std::tuple{current != nullptr, current && current->running, current && current->processing,
						current && current->local_stop_pending, current && current->remote_stop_pending,
						current && current->remote_stop_unconfirmed, current && current->reconnect_pending,
						current && current->recovering_remote_turn, current && current->managed_cancellation_pending,
						current && current->worktree_launch_pending, current && static_cast<bool>(current->pending_agent_input),
						current && static_cast<bool>(current->pending_local_launch)};
				};
				const auto before_stop = stop_projection();
				if (!StopAcpSession(app, run.transcript_chat_id, AcpStopPurpose::Timeout))
				{
					changed = app.status_line != status_before_stop || stop_projection() != before_stop || changed;
					continue;
				}
				if (AcpStopInProgress(app, run.transcript_chat_id) ||
				    app.pending_chat_save_at_by_chat_id.contains(run.transcript_chat_id))
				{
					changed = app.status_line != status_before_stop || stop_projection() != before_stop || changed;
					continue;
				}
				// The confirmed Stop retained the transcript for excerpt capture; no second Stop.
				const std::string excerpt_before_finish = run.result_excerpt;
				FinishRun(app, run, "failed", "wall_clock_deadline_exceeded",
					"The managed agent run exceeded its 30-minute total elapsed-time limit.", true);
				changed = run.status != "running" || run.result_excerpt != excerpt_before_finish ||
				    app.status_line != status_before_stop || stop_projection() != before_stop || changed;
				continue;
			}

			if (session != nullptr && session->managed_cancellation_pending)
			{
				if (app.pending_chat_save_at_by_chat_id.contains(run.transcript_chat_id)) continue;
				const std::string run_id = run.id;
				if (ManagedSubagentControlService::ValidateCancellationOwnership(app, run_id) && AgentRunScheduler::CancelTree(app, run_id))
					changed = true;
				continue;
			}
			if (session != nullptr && run.expected_turn_serial > 0 &&
			    session->last_settled_turn_serial >= run.expected_turn_serial)
			{
				FinishRun(app, run, session->last_turn_outcome == "completed" ? "completed" : "failed",
				          session->last_turn_outcome == "completed" ? "" : "provider_turn_failed",
				          session->last_turn_error);
				changed = true;
				continue;
			}

			if (session != nullptr && session->managed_launch_attempted &&
			    !session->managed_cancellation_pending && !session->running &&
			    !session->processing && !session->reconnect_pending)
			{
				RecordProviderCrash(app, run.provider_id, now_epoch_ms);
				FinishRun(app, run, "failed", "provider_exited",
				          uam::strings::NonEmptyOrFallback(session->last_error, "The provider exited before the managed turn settled."));
				changed = true;
			}
		}

		std::size_t running = RunningAgentSlots(app);
		std::size_t remaining_queued = app.queued_agent_run_ids.size();
		while (running < kMaxRunningAgents && remaining_queued > 0 && !app.queued_agent_run_ids.empty())
		{
			--remaining_queued;
			const std::string id = std::move(app.queued_agent_run_ids.front());
			app.queued_agent_run_ids.pop_front();
			AgentRun* run = FindRun(app, id);
			if (run == nullptr || run->status != "queued")
				continue;
			if (run->managed_continuation)
			{
				app.queued_agent_run_ids.push_back(id);
				continue;
			}
			if (app.pending_agent_run_cancellation_ids.contains(id))
			{
				app.queued_agent_run_ids.push_back(id);
				continue;
			}
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

			const ExecutionRoutingDecision routing = AdmitExecutionRouting(app, *transcript);
			if (routing.status == ExecutionRoutingStatus::Waiting || routing.status == ExecutionRoutingStatus::Invalid)
			{
				app.queued_agent_run_ids.push_back(id);
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
		return ManagedParentContinuationService::Poll(app) || changed;
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
			result_out->delivery_id = run->managed_continuation ? run->managed_continuation->turns.back().delivery_id : uam::strings::NonEmptyOrFallback(run->result_delivery_id, run->id);
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
		if (!ManagedSubagentControlService::ValidateCancellationOwnership(app, run_id, error_out))
			return false;
		const auto preparing = app.managed_followups.find(run_id);
		if (preparing != app.managed_followups.end() && preparing->second && !preparing->second->replay)
		{
			if (!FollowupTaskScope(app, *preparing->second)) { if (error_out) *error_out = "The managed follow-up cancellation owner changed."; return false; }
			CancelPendingInitialMention(app, preparing->second->transcript_id);
			if (error_out) *error_out = "Cancellation is waiting for managed preparation to retire.";
			return false;
		}
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
		for (const std::string& id : tree)
			if (!ManagedSubagentControlService::ValidateCancellationOwnership(app, id, error_out))
				return false;
		std::vector<AgentRun*> runs;
		for (AgentRun& run : app.agent_runs) if (tree.contains(run.id) && !IsTerminal(run.status)) runs.push_back(&run);
		for (const AgentRun* run : runs) app.pending_agent_run_cancellation_ids.insert(run->id);
		std::ranges::sort(runs, std::greater{}, &AgentRun::depth);
		bool success = true;
		std::string first_error;
		for (AgentRun* run : runs)
		{
			if (run->managed_continuation)
			{
				const auto owner = app.managed_initial_runtime_owners.find(run->id);
				if (owner == app.managed_initial_runtime_owners.end() || !InitialRuntimeOwned(app, *owner->second, false))
				{
					success = false;
					if (first_error.empty())
						first_error = "The exact managed initial cancellation owner is unavailable.";
					continue;
				}
				RequestInitialStop(*owner->second, "cancelled", "user_cancelled", "The user cancelled this agent run.");
				std::erase(app.queued_agent_run_ids, run->id);
				const auto* stopped_session = FindAcpSessionForChat(app, run->transcript_chat_id);
				const auto* stopped_chat = ChatDomainService().FindChatById(app, run->transcript_chat_id);
				// Cancellation retries must preserve the finalized resume transcript used by terminal CAS.
				if (!owner->second->resume_only || !stopped_session || !stopped_chat || !InitialReadOnlyStoppedLeaseOwned(app, *owner->second, *stopped_session, *stopped_chat))
					(void)StopAcpSession(app, run->transcript_chat_id);
				success = false;
				if (first_error.empty())
					first_error = "Cancellation is waiting for the provider to stop and the result to be saved.";
				continue;
			}
			AcpSessionState* session = FindAcpSessionForChat(app, run->transcript_chat_id);
			if (app.pending_chat_save_at_by_chat_id.contains(run->transcript_chat_id))
			{
				if (session != nullptr) session->managed_cancellation_pending = true;
				success = false;
				if (first_error.empty()) first_error = "The remote stop is saved locally but its recovery marker is still being persisted.";
				continue;
			}
			if (session != nullptr) session->managed_cancellation_pending = false;
			if (!StopAcpSession(app, run->transcript_chat_id))
			{
				session = FindAcpSessionForChat(app, run->transcript_chat_id);
				if (session != nullptr && !session->running &&
				    (session->remote_stop_pending ||
				     app.pending_chat_save_at_by_chat_id.contains(run->transcript_chat_id)))
				{
					session->managed_cancellation_pending = true;
				}
				success = false;
				if (first_error.empty())
					first_error = uam::strings::NonEmptyOrFallback(
					    app.status_line, "The managed provider could not be stopped safely.");
				continue;
			}
			if (!PersistStatus(app, *run, "cancelled", "user_cancelled", "The user cancelled this agent run."))
			{
				session = FindAcpSessionForChat(app, run->transcript_chat_id);
				if (session != nullptr) session->managed_cancellation_pending = true;
				success = false;
				if (first_error.empty())
					first_error = "One or more cancellation records could not be persisted.";
				continue;
			}
			app.pending_agent_run_cancellation_ids.erase(run->id);
			app.managed_subagent_cancellation_receipts.erase(run->id);
			app.agent_run_deadline_steady_ms.erase(run->id);
			EraseRuntimeChat(app, run->transcript_chat_id);
		}
		std::erase_if(app.queued_agent_run_ids, [&](const std::string& id) { return tree.contains(id) && !app.pending_agent_run_cancellation_ids.contains(id); });
		if (!success && error_out != nullptr) *error_out = std::move(first_error);
		return success;
	}

	bool AgentRunScheduler::InterruptForShutdown(AppState& app)
	{
		std::vector<std::string> pending_cancellations;
		for (const AgentRun& run : app.agent_runs)
		{
			const AcpSessionState* session = FindAcpSessionForChat(app, run.transcript_chat_id);
			if (!IsTerminal(run.status) && session != nullptr &&
			    session->managed_cancellation_pending && !session->remote_stop_pending &&
			    !app.pending_chat_save_at_by_chat_id.contains(run.transcript_chat_id))
				pending_cancellations.push_back(run.id);
		}
		for (const std::string& run_id : pending_cancellations)
			(void)CancelTree(app, run_id);
		std::vector<std::string> errors;
		std::vector<AgentRun> admitted;
		for (const AgentRun& run : app.agent_runs)
			if (!run.managed_continuation && ManagedSubagentControlService::ValidateCancellationOwnership(app, run.id))
				admitted.push_back(run);
		const bool interrupted = AgentRunLedger::MarkNonterminalInterrupted(app.data_root, &admitted, "manager_shutdown", &errors);
		for (AgentRun& saved : admitted)
			if (AgentRun* current = FindRun(app, saved.id))
				*current = std::move(saved);
		app.agent_run_deadline_steady_ms.clear();
		return interrupted;
	}
} // namespace uam
