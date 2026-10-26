#include <algorithm>
#include <cmath>
#include <utility>
#include "common/runtime/acp/claude_quota_continuation.h"
// Builds without this provider use the inline stubs in the header; test targets may still list this file.
#if UAM_ENABLE_RUNTIME_CLAUDE_CLI

#include "app/chat_domain_service.h"
#include "app/execution_routing_integration.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_availability.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/time_utils.h"
#include "common/utils/hash_utils.h"

namespace uam
{
	struct ClaudeQuotaHistoryTask
	{
		std::shared_ptr<ChatDomainService::ColdHistoryPreparation> preparation;
		std::string owner_id;
		std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<bool> loaded = std::make_shared<bool>(false);
		std::unique_ptr<std::jthread> worker;
	};
}

namespace uam::acp_detail
{
	namespace
	{
		bool ValidReset(const ClaudeQuotaContinuation& owner)
		{
			return owner.observed_at_epoch_s > 0 && owner.observed_at_epoch_s <= 253402282739 &&
			    owner.resets_at_epoch_s > owner.observed_at_epoch_s &&
			    owner.resets_at_epoch_s <= owner.observed_at_epoch_s + 18060;
		}

		std::string AcceptedMessageDigest(const Message& message)
		{
			uint64_t digest = hashing::kFnv1a64OffsetBasis;
			hashing::UpdateFnv1a64WithSeparator(digest, message.content);
			hashing::UpdateFnv1a64WithSeparator(digest, message.created_at);
			for (const MessageAttachment& attachment : message.attachments)
			{
				hashing::UpdateFnv1a64WithSeparator(digest, attachment.path);
				hashing::UpdateFnv1a64WithSeparator(digest, attachment.kind);
			}
			return hashing::Hex64Padded(digest);
		}

		bool MatchesOwner(const AppState& app, const ChatSession& chat)
		{
			const ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
			return chat.provider_id == "claude-cli" && !chat.imported_read_only && !chat.native_session_reset_pending &&
			    !owner.id.empty() && owner.chat_id == chat.id && !owner.native_session_id.empty() &&
			    owner.native_session_id == chat.native_session_id && owner.native_session_id == ResolvedAcpResumeIdForChat(app, chat) &&
			    owner.context == ClaudeQuotaContext(app, chat) && ValidReset(owner) && owner.user_message_index >= 0 &&
			    owner.message_count == (chat.messages_loaded ? chat.messages.size() : chat.persisted_message_count);
		}

		bool QuotaRuntimeAvailable(const AppState& app, const ChatSession& chat, const AcpSessionState* session, bool cold_settlement = false)
		{
			return !app.managed_initial_shutdown && !AcpStopInProgress(app, chat.id) && !chat.side_cleanup_requested &&
			    !chat.acp_queue_held && !chat.acp_queue_storage_error &&
			    !chat.remote_turn_reconnect_pending && !chat.remote_stop_cleanup_pending && !chat.remote_restart_pending &&
			    !app.worktree_operation_chat_ids.contains(chat.id) &&
			    (!session || (session->provider_id == chat.provider_id && session->transport_ownership_error.empty() &&
			     (!session->running || (session->session_id == chat.native_session_id && session->process_execution_host_id == chat.execution_host_id)) &&
			     !AcpSessionHasPendingCancel(*session) && !AcpSessionIsWaitingForInput(*session) &&
			     !session->manual_compaction_pending && !session->context_compaction_in_progress &&
			     !session->pending_compaction && !session->turn_checkpoint_commit_pending && !session->local_stop_pending &&
			     !session->remote_stop_pending && !session->remote_stop_unconfirmed && !session->queue_held &&
			     !session->remote_source_exit_reported && !session->recovering_remote_turn && !session->recovering_remote_process &&
			     !session->pending_agent_input && !session->pending_queued_history && (!session->pending_local_launch || cold_settlement) &&
			     !session->pending_local_session_receipt && (!session->worktree_launch_pending || cold_settlement) &&
			     (!session->reconnect_pending || cold_settlement) && session->claude_live_background_task_ids.empty()));
		}

		struct QuotaPublication
		{
			std::filesystem::path root;
			std::string id, queue, runtime_scope, prompt, host, token;
			std::shared_ptr<const std::uint8_t> incarnation;
			ClaudeQuotaContinuation owner;
			std::optional<GoalPromptReceiptScope> goal_scope;
			std::deque<AcpQueuedUserPromptState> raw_queue;
			const AcpSessionState* session = nullptr;
			std::shared_ptr<platform::AsyncByteWriter> writer;
			int serial = 0, protocol = 0, user_index = -1;
			std::optional<Message> input;
			bool processing = false;
			bool attempted = false;
			bool worktree_pending = false, reconnect_pending = false;
			std::shared_ptr<std::stop_source> worktree_cancellation;
			std::shared_ptr<PendingLocalAcpLaunch> launch;
		};

		std::shared_ptr<QuotaPublication> CaptureQuotaPublication(const AppState& app, const ChatSession& chat)
		{
			auto scope = std::make_shared<QuotaPublication>();
			scope->root = app.data_root;
			scope->id = chat.id;
			scope->incarnation = chat.metadata_incarnation;
			scope->owner = chat.claude_quota_continuation;
			scope->queue = ChatRepository::QueueAdmissionFingerprint(chat);
			scope->session = FindAcpSessionForChat(app, chat.id);
			if (!scope->session)
			{
				AcpSessionState absent;
				absent.provider_id = chat.provider_id;
				if (chat.small_model_mode) scope->goal_scope = CaptureGoalPromptReceiptScope(app, absent, chat);
				scope->runtime_scope = LocalSessionReceiptOwnership(app, absent, chat);
			}
			if (scope->session)
			{
				if (chat.small_model_mode) scope->goal_scope = CaptureGoalPromptReceiptScope(app, *scope->session, chat);
				scope->runtime_scope = LocalSessionReceiptOwnership(app, *scope->session, chat);
				scope->worktree_pending = scope->session->worktree_launch_pending;
				scope->worktree_cancellation = scope->session->worktree_launch_cancellation;
				scope->reconnect_pending = scope->session->reconnect_pending;
				scope->launch = scope->session->pending_local_launch;
				scope->writer = scope->session->stdin_writer;
				scope->raw_queue = scope->session->queued_user_prompts;
				scope->serial = scope->session->turn_serial;
				scope->processing = scope->session->processing;
				scope->prompt = scope->session->queued_prompt;
				scope->host = scope->session->process_execution_host_id;
				scope->token = scope->session->remote_output_delivery_token;
				scope->protocol = scope->session->remote_runner_protocol_version;
				scope->user_index = scope->session->turn_user_message_index;
				if (scope->processing && scope->user_index >= 0 && scope->user_index < static_cast<int>(chat.messages.size()))
					scope->input = chat.messages[scope->user_index];
			}
			return scope;
		}

		bool OwnsQuotaPublication(const AppState& app, const QuotaPublication& scope)
		{
			const auto* chat = ChatDomainService().FindChatById(app, scope.id);
			const auto* session = FindAcpSessionForChat(app, scope.id);
			if (!chat || app.data_root != scope.root || chat->metadata_incarnation != scope.incarnation ||
			    chat->claude_quota_continuation != scope.owner || chat->provider_id != "claude-cli" ||
			    chat->native_session_id != scope.owner.native_session_id || chat->native_session_reset_pending ||
			    chat->imported_read_only || !provider_availability::IsEnabled(app.settings, chat->provider_id, chat->execution_host_id) ||
			    ResolvedAcpResumeIdForChat(app, *chat) != scope.owner.native_session_id ||
			    ClaudeQuotaContext(app, *chat) != scope.owner.context || !chat->messages_loaded ||
			    scope.owner.user_message_index < 0 || scope.owner.user_message_index >= static_cast<int>(chat->messages.size()) ||
			    chat->messages[scope.owner.user_message_index].role != MessageRole::User ||
			    chat->messages[scope.owner.user_message_index].acp_prompt_not_sent ||
			    AcceptedMessageDigest(chat->messages[scope.owner.user_message_index]) != scope.owner.accepted_message_digest ||
			    ChatRepository::QueueAdmissionFingerprint(*chat) != scope.queue || session != scope.session ||
			    (chat->small_model_mode && !scope.goal_scope) ||
			    !QuotaRuntimeAvailable(app, *chat, session, scope.processing && session && !session->running)) return false;
			if (!session)
			{
				AcpSessionState absent;
				absent.provider_id = chat->provider_id;
				return !scope.processing && (!chat->small_model_mode ||
				    CaptureGoalPromptReceiptScope(app, absent, *chat) == scope.goal_scope) &&
				    LocalSessionReceiptOwnership(app, absent, *chat) == scope.runtime_scope;
			}
			return (!session->running || (session->session_ready &&
			    (chat->execution_host_id == execution_hosts::kLocalHostId || ValidateAcpRestartOwnership(app, *session, *chat, nullptr)))) &&
			    session->prompt_request_id == 0 && session->worktree_launch_pending == scope.worktree_pending &&
			    session->worktree_launch_cancellation == scope.worktree_cancellation &&
			    (!scope.worktree_cancellation || !scope.worktree_cancellation->stop_requested()) &&
			    session->reconnect_pending == scope.reconnect_pending && session->pending_local_launch == scope.launch &&
			    session->turn_serial == scope.serial && session->processing == scope.processing &&
			    session->queued_prompt == scope.prompt && session->stdin_writer == scope.writer && session->queued_user_prompts == scope.raw_queue &&
			    session->process_execution_host_id == scope.host && session->remote_output_delivery_token == scope.token &&
			    session->remote_runner_protocol_version == scope.protocol &&
			    (!chat->small_model_mode || CaptureGoalPromptReceiptScope(app, *session, *chat) == scope.goal_scope) &&
			    (!scope.input || (session->turn_user_message_index == scope.user_index && scope.user_index >= 0 &&
			     scope.user_index < static_cast<int>(chat->messages.size()) && chat->messages[scope.user_index] == *scope.input)) &&
			    (!scope.processing || (session->claude_quota_delivery_id == scope.owner.id && ValidateClaudeQuotaDelivery(app, *session, *chat))) &&
			    !session->pending_prompt_delivery && !AcpSessionHasPendingRuntimeRequest(*session) &&
			    LocalSessionReceiptOwnership(app, *session, *chat) == scope.runtime_scope;
		}

		void HoldQuotaPublication(AppState& app, const QuotaPublication& scope)
		{
			auto* chat = ChatDomainService().FindChatById(app, scope.id);
			if (!chat || app.data_root != scope.root || chat->metadata_incarnation != scope.incarnation ||
			    chat->claude_quota_continuation != scope.owner) return;
			chat->claude_quota_continuation.state = "held";
			ScheduleChatSave(app, *chat, 0.0);
		}

		bool CanStageQuotaPublication(const AppState& app, const ChatSession& chat, const AcpSessionState* session)
		{
			if (!session || !session->running) return true;
			if (!session->session_ready) return false;
			if (CanDeferRemotePromptAdmission(app, *session, chat) || CanStageRemoteGoalPromptAdmission(app, *session, chat)) return true;
			if (chat.execution_host_id != execution_hosts::kLocalHostId) return false;
#if defined(_WIN32)
			return session->stdin_write != INVALID_HANDLE_VALUE && session->stdin_write != nullptr;
#else
			return session->stdin_write_fd >= 0;
#endif
		}

		void StageQuotaSettlement(AppState& app, ChatSession& chat)
		{
			const auto scope = CaptureQuotaPublication(app, chat);
			const auto owned = [&app, scope]() { return OwnsQuotaPublication(app, *scope); };
			const auto complete = [&app, scope](DurableAdmissionState state)
			{
				if (state == DurableAdmissionState::Committed) return;
				// Even a root/metadata replacement must retire this exact unattempted runtime input;
				// it must not rebase itself onto the next save once the failed task leaves the map.
				auto* session = FindAcpSessionForChat(app, scope->id);
				if (scope->processing && session == scope->session && session->turn_serial == scope->serial &&
				    session->prompt_request_id == 0 && session->queued_prompt == scope->prompt &&
				    session->claude_quota_delivery_id == scope->owner.id)
				{
					session->last_error = "Native quota continuation lost its owned settlement receipt.";
					session->goal_resume_suppressed = true;
					CompletePromptTurn(*session, kAcpLifecycleError);
				}
				HoldQuotaPublication(app, *scope);
			};
			// This owns the newly admitted continuation, so a failed full save rejects that exact unsent turn.
			if (BeginDurableAdmission(app, chat, owned, complete, DurableAdmissionFailurePurpose::Prompt) != DurableAdmissionState::Pending)
				complete(DurableAdmissionState::Failed);
			PinDurableAdmissionRoot(app, chat.id);
		}

		void DispatchQuotaClaim(AppState& app, ChatSession& chat, const std::string& owner_id, bool staged)
		{
			const auto id = chat.id;
			const auto incarnation = chat.metadata_incarnation;
			const auto root = app.data_root;
			const auto claimed = chat.claude_quota_continuation;
			bool sent = false;
			{
				struct DispatchScope
				{
					std::string& value;
					std::string previous;
					~DispatchScope() { value = std::move(previous); }
				} dispatch_scope{app.claude_quota_dispatch_id, app.claude_quota_dispatch_id};
				app.claude_quota_dispatch_id = owner_id;
				std::string error;
				sent = StartClaudeQuotaContinuation(app, chat, &error);
			}
			auto* current = ChatDomainService().FindChatById(app, id);
			if (!current || app.data_root != root || current->metadata_incarnation != incarnation ||
			    current->claude_quota_continuation != claimed || current->native_session_id != claimed.native_session_id ||
			    current->claude_quota_continuation.state != "claimed") return;
			current->claude_quota_continuation.state = sent ? "continued" : "held";
			if (sent) current->claude_quota_continuation.context = ClaudeQuotaContext(app, *current);
			auto* native = FindAcpSessionForChat(app, id);
			if (sent && native && HasPendingAcpColdLaunch(*native))
				BindPendingAcpColdLaunch(app, *native, *current, true); // Settlement and cold authority share a full receipt.
			else if (staged && CanStageQuotaPublication(app, *current, native))
			{
				if (sent && native && !RebindPendingAcpQuotaConfiguration(app, *native, *current))
				{
					native->last_error = "Quota launch configuration changed before settlement.";
					CompletePromptTurn(*native, kAcpLifecycleError);
					current->claude_quota_continuation.state = "held";
					ScheduleChatSave(app, *current, 0.0);
				}
				else StageQuotaSettlement(app, *current);
			}
			else (void)SaveChatQuietly(app, *current); // Legacy/restarting transport keeps its separate control barrier.
		}

		void StageQuotaClaim(AppState& app, ChatSession& chat)
		{
			const auto scope = CaptureQuotaPublication(app, chat);
			const auto owned = [&app, scope]()
			{
				return !scope->attempted && OwnsQuotaPublication(app, *scope) &&
				    ChatDomainService().FindChatById(app, scope->id)->messages.size() == scope->owner.message_count;
			};
			const auto complete = [&app, scope, owned](DurableAdmissionState state)
			{
				if (state != DurableAdmissionState::Committed || !owned()) { HoldQuotaPublication(app, *scope); return; }
				scope->attempted = true; // Mark before dispatch; a save callback is never retry authority.
				DispatchQuotaClaim(app, *ChatDomainService().FindChatById(app, scope->id), scope->owner.id, true);
			};
			if (BeginDurableAdmission(app, chat, owned, complete, DurableAdmissionFailurePurpose::BindingOnly) != DurableAdmissionState::Pending)
				complete(DurableAdmissionState::Failed);
			PinDurableAdmissionRoot(app, chat.id);
		}
	}

	std::string ClaudeQuotaContext(const AppState& app, const ChatSession& chat)
	{
		const ExecutionHost* host = execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id);
		const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(app, chat);
		return nlohmann::json::array({chat.execution_host_id, host == nullptr ? "" : ExecutionRoutingConnectionIdentity(*host),
		    provider == nullptr ? "" : ExecutionRoutingProviderIdentity(app, *provider, chat.execution_host_id),
		    paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat)), chat.model_id, chat.approval_mode,
		    chat.uam_agent_id, chat.last_prompt_agent_definition_hash, chat.goal_command_revision}).dump();
	}

	bool IsCurrentClaudeQuotaContinuationInput(const AppState& app, const AcpSessionState& session, const ChatSession& chat, const AcpQueuedUserPromptState& input)
	{
		const ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
		return session.provider_id == "claude-cli" && chat.provider_id == session.provider_id &&
		    !app.claude_quota_dispatch_id.empty() && app.claude_quota_dispatch_id == owner.id && owner.state == "claimed" &&
		    owner.chat_id == chat.id && !owner.native_session_id.empty() &&
		    owner.native_session_id == session.session_id && owner.native_session_id == chat.native_session_id &&
		    owner.context == ClaudeQuotaContext(app, chat) && input.text == kClaudeQuotaContinuationPrompt &&
		    input.uam_agent_id == owner.agent_id && input.uam_agent_definition_hash == owner.agent_definition_hash &&
		    input.uam_agent_workspace_access == owner.agent_workspace_access &&
		    input.uam_agent_execution_capability == owner.agent_execution_capability;
	}

	void ObserveClaudeQuota(AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message, int64_t now_epoch_s)
	{
		if (!session.processing || session.turn_serial <= 0 || !session.queued_prompt.empty() || AcpSessionHasPendingCancel(session) ||
		    session.goal_internal_session || session.model_discovery_only || session.provider_id != "claude-cli" ||
		    JsonDiagnosticStringValue(message, "session_id") != session.session_id || session.session_id != chat.native_session_id)
			return;
		// Each authoritative event replaces older evidence. Never infer reset times from prose or retry delays.
		session.claude_quota_reset_epoch_s = 0;
		session.claude_quota_evidence_turn_serial = 0;
		const nlohmann::json info = JsonObjectValue(message, "rate_limit_info");
		if (JsonDiagnosticStringValue(info, "status") != "rejected" || JsonDiagnosticStringValue(info, "rateLimitType") != "five_hour") return;
		const nlohmann::json reset = info.value("resetsAt", nlohmann::json());
		if (!reset.is_number_integer() || (reset.is_number_unsigned() && reset.get<uint64_t>() > 253402300799)) return;
		const int64_t reset_s = reset.get<int64_t>();
		ClaudeQuotaContinuation candidate;
		candidate.observed_at_epoch_s = now_epoch_s;
		candidate.resets_at_epoch_s = reset_s;
		if (!ValidReset(candidate)) return;
		// unifiedWindows is an object keyed by window name, not an array of status records.
		if (JsonDiagnosticStringValue(info, "overageStatus") == "rejected") return;
		if (info.contains("unifiedWindows"))
		{
			const nlohmann::json& windows = info["unifiedWindows"];
			if (!windows.is_object()) return;
			for (nlohmann::json::const_iterator entry = windows.begin(); entry != windows.end(); ++entry)
			{
				const std::string& name = entry.key();
				const nlohmann::json& window = entry.value();
				if (!window.is_object() || !window.contains("utilization") || !window["utilization"].is_number()) return;
				const double utilization = window["utilization"].get<double>();
				if (!std::isfinite(utilization) || utilization < 0 || (name != "five_hour" && utilization >= 1)) return;
				if (name == "five_hour" && window.value("resetsAt", nlohmann::json()) != reset) return;
			}
		}
		session.claude_quota_reset_epoch_s = reset_s;
		session.claude_quota_observed_epoch_s = now_epoch_s;
		session.claude_quota_evidence_turn_serial = session.turn_serial;
	}

	bool OwnClaudeQuotaWait(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& result, int64_t now_epoch_s)
	{
		const bool usage_error = std::exchange(session.claude_quota_usage_error, false);
		const int64_t reset_s = std::exchange(session.claude_quota_reset_epoch_s, 0);
		const int evidence_turn = std::exchange(session.claude_quota_evidence_turn_serial, 0);
		if (!usage_error || reset_s == 0 || evidence_turn != session.turn_serial || !session.processing || session.turn_serial <= 0 || !session.queued_prompt.empty() ||
		    AcpSessionHasPendingCancel(session) || JsonDiagnosticStringValue(result, "session_id") != chat.native_session_id ||
		    session.session_id != chat.native_session_id || JsonDiagnosticStringValue(result, "subtype") != "error_during_execution" ||
		    !JsonBooleanValueOr(result, "is_error", false) || now_epoch_s < session.claude_quota_observed_epoch_s ||
		    !chat.messages_loaded || session.turn_user_message_index < 0 || session.turn_user_message_index >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[session.turn_user_message_index].role != MessageRole::User || chat.messages[session.turn_user_message_index].acp_prompt_not_sent)
			return false;
		ClaudeQuotaContinuation owner;
		owner.id = time::SteadyEpochNanosecondsTokenNow();
		owner.state = "waiting";
		owner.chat_id = chat.id;
		owner.native_session_id = chat.native_session_id;
		owner.context = ClaudeQuotaContext(app, chat);
		owner.agent_id = session.active_uam_agent_id;
		owner.agent_definition_hash = session.active_uam_agent_definition_hash;
		owner.agent_workspace_access = session.active_uam_agent_workspace_access;
		owner.agent_execution_capability = session.active_uam_agent_execution_capability;
		owner.observed_at_epoch_s = session.claude_quota_observed_epoch_s;
		owner.resets_at_epoch_s = reset_s;
		owner.user_message_index = session.turn_user_message_index;
		owner.accepted_message_digest = AcceptedMessageDigest(chat.messages[owner.user_message_index]);
		// Unresolved tools and managed deadlines need explicit recovery, never an automatic new turn.
		if (!session.claude_quota_delivery_id.empty() || !chat.agent_run_id.empty() || !session.claude_live_background_task_ids.empty() ||
		    std::ranges::any_of(session.tool_calls, [](const AcpToolCallState& tool)
		    { return tool.status != acp_statuses::kCompleted && tool.status != acp_statuses::kCancelled; })) owner.state = "held";
		chat.claude_quota_continuation = std::move(owner);
		return true;
	}

	bool ValidateClaudeQuotaDelivery(const AppState& app, const AcpSessionState& session, const ChatSession& chat)
	{
		if (session.claude_quota_delivery_id.empty()) return true;
		const ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
		return session.claude_quota_delivery_id == owner.id && (owner.state == "claimed" || owner.state == "continued") &&
		    owner.chat_id == chat.id && owner.native_session_id == chat.native_session_id && owner.native_session_id == session.session_id &&
		    owner.agent_id == session.active_uam_agent_id && owner.agent_definition_hash == session.active_uam_agent_definition_hash &&
		    owner.agent_workspace_access == session.active_uam_agent_workspace_access && owner.agent_execution_capability == session.active_uam_agent_execution_capability &&
		    chat.messages_loaded && owner.user_message_index >= 0 && owner.user_message_index < static_cast<int>(chat.messages.size()) &&
		    chat.messages[owner.user_message_index].role == MessageRole::User &&
		    AcceptedMessageDigest(chat.messages[owner.user_message_index]) == owner.accepted_message_digest &&
		    chat.messages.size() == owner.message_count + 1 &&
		    !chat.native_session_reset_pending && owner.context == ClaudeQuotaContext(app, chat) &&
		    owner.native_session_id == ResolvedAcpResumeIdForChat(app, chat);
	}

	bool CancelClaudeQuotaWait(AppState& app, ChatSession& chat)
	{
		ClaudeQuotaContinuation& owner = chat.claude_quota_continuation;
		if (owner.state == "cancelled")
			return SaveChatQuietly(app, chat);
		if (owner.id.empty() || owner.state == "completed") return true;
		owner.state = "cancelled";
		if (AcpSessionState* session = FindAcpSessionForChat(app, chat.id))
		{
			session->claude_quota_reset_epoch_s = 0;
			session->claude_quota_evidence_turn_serial = 0;
		}
		// Keep the in-memory tombstone on save failure; no wake is allowed while persistence is retried.
		return SaveChatQuietly(app, chat);
	}

	bool PrepareClaudeQuotaCancellationForManualAdmission(AppState& app, ChatSession& chat)
	{
		AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
		const bool waiting = chat.claude_quota_continuation.state == "waiting" || chat.claude_quota_continuation.state == "cancelled";
		const bool pending_transport = session == nullptr || !session->running || !session->session_ready;
		bool usable = session != nullptr && session->running && session->session_ready;
#if defined(_WIN32)
		usable = usable && session->stdin_write != INVALID_HANDLE_VALUE && session->stdin_write != nullptr;
#else
		usable = usable && session->stdin_write_fd >= 0;
#endif
		const bool remote_admission = usable && session->transport_ownership_error.empty() &&
		    (CanDeferRemotePromptAdmission(app, *session, chat) || CanStageRemoteGoalPromptAdmission(app, *session, chat));
		if (!waiting || chat.provider_id != "claude-cli" ||
		    (chat.execution_host_id == execution_hosts::kLocalHostId ? (!pending_transport && !usable) : !remote_admission) ||
		    !app.claude_quota_dispatch_id.empty()) return CancelClaudeQuotaWait(app, chat);
		// Local, ordinary v3 and separately goal-owned remote admission stage full input before dispatch.
		// This tombstone stops the in-memory scheduler; the subsequent full receipt makes it durable.
		chat.claude_quota_continuation.state = "cancelled";
		if (session != nullptr)
		{
			session->claude_quota_reset_epoch_s = 0;
			session->claude_quota_evidence_turn_serial = 0;
		}
		ScheduleChatSave(app, chat, 0.0);
		return true;
	}

	bool PollClaudeQuotaContinuations(AppState& app, int64_t now_epoch_s)
	{
		bool changed = false;
		for (const std::shared_ptr<ClaudeQuotaHistoryTask>& task : app.claude_quota_history_tasks)
		{
			if (!task->finished->load(std::memory_order_acquire)) continue;
			ChatSession* chat = ChatDomainService().FindChatById(app, task->preparation->identity.chat_id);
			if (chat != nullptr && chat->claude_quota_continuation.id == task->owner_id &&
			    chat->claude_quota_continuation.state == "waiting" && MatchesOwner(app, *chat) && *task->loaded)
				changed = ChatDomainService().CompleteColdHistoryPreparation(app, *task->preparation) || changed;
			else
			{
				(void)ChatDomainService().ConsumeBranchPreparation(app, task->preparation->identity.chat_id, task->preparation->ownership);
				if (chat != nullptr && chat->claude_quota_continuation.id == task->owner_id && chat->claude_quota_continuation.state == "waiting")
				{
					chat->claude_quota_continuation.state = "held";
					(void)SaveChatQuietly(app, *chat);
					changed = true;
				}
			}
		}
		std::erase_if(app.claude_quota_history_tasks, [](const std::shared_ptr<ClaudeQuotaHistoryTask>& task)
		{ return task->finished->load(std::memory_order_acquire); });
		// Native startup can change app.chats. Iterate stable IDs, then resolve after each mutation.
		std::vector<std::string> ids;
		for (const ChatSession& chat : app.chats)
			if (chat.claude_quota_continuation.state == "waiting") ids.push_back(chat.id);
		for (const std::string& id : ids)
		{
			ChatSession* chat = ChatDomainService().FindChatById(app, id);
			if (chat == nullptr) continue;
			if (!MatchesOwner(app, *chat))
			{
				(void)CancelClaudeQuotaWait(app, *chat);
				changed = true;
				continue;
			}
			if (now_epoch_s < chat->claude_quota_continuation.resets_at_epoch_s) continue;
			if (!chat->messages_loaded)
			{
				if (app.claude_quota_history_tasks.size() >= 2) continue;
				const std::shared_ptr<ChatDomainService::ColdHistoryPreparation> preparation = ChatDomainService().BeginColdHistoryPreparation(app, id);
				if (!preparation) continue;
				const std::shared_ptr<ClaudeQuotaHistoryTask> task = std::make_shared<ClaudeQuotaHistoryTask>();
				task->preparation = preparation;
				task->owner_id = chat->claude_quota_continuation.id;
				task->worker = std::make_unique<std::jthread>([preparation, loaded = task->loaded, finished = task->finished](std::stop_token)
				{
					std::string error;
					try { *loaded = ChatDomainService().LoadColdHistoryPreparation(*preparation, error); }
					catch (...) { *loaded = false; }
					finished->store(true, std::memory_order_release);
				});
				app.claude_quota_history_tasks.push_back(task);
				continue;
			}
			const ClaudeQuotaContinuation& owner = chat->claude_quota_continuation;
			if (owner.user_message_index >= static_cast<int>(chat->messages.size()) ||
			    chat->messages[owner.user_message_index].role != MessageRole::User || chat->messages[owner.user_message_index].acp_prompt_not_sent ||
			    AcceptedMessageDigest(chat->messages[owner.user_message_index]) != owner.accepted_message_digest)
			{
				(void)CancelClaudeQuotaWait(app, *chat);
				changed = true;
				continue;
			}
			const AcpSessionState* session = FindAcpSessionForChat(app, id);
			if (app.pending_chat_save_at_by_chat_id.contains(id) || !QuotaRuntimeAvailable(app, *chat, session)) continue;
			if (chat->execution_host_id == execution_hosts::kLocalHostId)
			{
				const std::unordered_map<std::string, CliProviderVersionState>::const_iterator version = app.runtime_cli_versions_by_provider_id.find("claude-cli");
				if (version == app.runtime_cli_versions_by_provider_id.end() || !version->second.checked ||
				    !version->second.supported || !version->second.check_error.empty()) continue;
			}
			if (AcpStopInProgress(app, id) || (session != nullptr && (session->processing || !session->queued_prompt.empty() ||
			    AcpSessionHasPendingRuntimeRequest(*session) || AcpSessionIsWaitingForInput(*session) ||
			    session->remote_stop_pending || session->remote_stop_unconfirmed ||
			    !session->claude_live_background_task_ids.empty() || session->queue_held ||
			    (session->running && (!session->session_ready || session->session_id != owner.native_session_id))))) continue;
			if (chat->remote_turn_reconnect_pending || chat->remote_stop_cleanup_pending || chat->remote_restart_pending || chat->acp_queue_held) continue;
			const bool staged = CanStageQuotaPublication(app, *chat, session);
			const std::string owner_id = owner.id;
			chat->claude_quota_continuation.state = "claimed";
			if (staged) StageQuotaClaim(app, *chat);
			else
			{
				if (!SaveChatQuietly(app, *chat)) { chat->claude_quota_continuation.state = "waiting"; continue; }
				DispatchQuotaClaim(app, *chat, owner_id, false);
			}
			changed = true;
		}
		return changed;
	}
}
#endif // UAM_ENABLE_RUNTIME_CLAUDE_CLI
